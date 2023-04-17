#pragma once
#include <fstream>
#include <cstring>
#include <unistd.h> //open and close
#include <fcntl.h>
#include "graph.h"

class BadStreamException : public std::exception {
  virtual const char* what() const throw() {
    return "The stream file was not correctly opened. Does it exist?";
  }
};

class StreamFailedException : public std::exception {
  virtual const char* what() const throw() {
    return "ERROR: read_data() encountered failed stream. Is stream file corrupted?";
  }
};

// A class for reading from a binary graph stream
class BinaryGraphStream {
public:
  BinaryGraphStream(std::string file_name, uint32_t _b) {
    bin_file.open(file_name.c_str(), std::ios_base::in | std::ios_base::binary);

    if (!bin_file.is_open()) {
      throw BadStreamException();
    }
    // set the buffer size to be a multiple of an edge size and malloc memory
    buf_size = _b - (_b % edge_size);
    buf = (char *) malloc(buf_size * sizeof(char));
    start_buf = buf;

    // read header from the input file
    bin_file.read(reinterpret_cast<char *>(&num_nodes), 4);
    bin_file.read(reinterpret_cast<char *>(&num_edges), 8);
    
    read_data(); // read in the first block of data
  }
  ~BinaryGraphStream() {
    free(start_buf);
  }
  inline uint32_t nodes() {return num_nodes;}
  inline uint64_t edges() {return num_edges;}

  inline GraphUpdate get_edge() {
    UpdateType u = (UpdateType) *buf;
    uint32_t a;
    uint32_t b;

    std::memcpy(&a, buf + 1, sizeof(uint32_t));
    std::memcpy(&b, buf + 5, sizeof(uint32_t));
    
    buf += edge_size;
    if (buf - start_buf == buf_size) read_data();

    return {{a,b}, u};
  }

private:
  inline void read_data() {
    // set buf back to the beginning of the buffer read in data
    buf = start_buf;
    bin_file.read(buf, buf_size);
  
    if (bin_file.fail() && !bin_file.eof()) {
      throw StreamFailedException();
    }  
  }
  const uint32_t edge_size = sizeof(uint8_t) + 2 * sizeof(uint32_t); // size of binary encoded edge
  std::ifstream bin_file; // file to read from
  char *buf;              // data buffer
  char *start_buf;        // the start of the data buffer
  uint32_t buf_size;      // how big is the data buffer
  uint32_t num_nodes;     // number of nodes in the graph
  uint64_t num_edges;     // number of edges in the graph stream
};

// Class for reading from a binary graph stream using many
// MT_StreamReader threads
class BinaryGraphStream_MT {
public:
  BinaryGraphStream_MT(std::string file_name, uint32_t _b) {
    stream_fd = open(file_name.c_str(), O_RDONLY, S_IRUSR);
    if (!stream_fd) {
      throw BadStreamException();
    }

    // set the buffer size to be a multiple of an edge size
    buf_size = _b - (_b % edge_size);

    // read header from the input file
    if (read(stream_fd, reinterpret_cast<char *>(&num_nodes), 4) != 4)
      throw BadStreamException();
    if (read(stream_fd, reinterpret_cast<char *>(&num_edges), 8) != 8)
      throw BadStreamException();
    end_of_file = (num_edges * edge_size) + header_size;
    query_index = -1;
    stream_off = header_size;
    query_block = false;
  }

  /* Call this function to ask stream to pause so we can perform a query
   * This allows queries to be performed in the stream arbitrary at 32 KiB granularity
   * without advance notice
   *
   * IMPORTANT: When perfoming queries it is the responsibility of the user to ensure that 
   * all threads have finished processing their current work. This is indicated by all threads 
   * pulling data from the stream returning BREAKPOINT.
   * If all threads do not return BREAKPOINT then not all updates have been applied to the graph.
   * Threads must not call get_edge while the query is in progress otherwise edge cases can occur.
   * This is true for both on_demand and registered queries.
   */
  void on_demand_query() { query_block = true; }

  // call this function to tell stream its okay to keep going
  // call once per query performed regardless if registered query or on-demand query
  void post_query_resume() { query_block = false; query_index = -1; }

  /*
   * Call this function to register a query in advance to avoid constraint on 32 KiB boundary
   * Only one query may be registered at a time and query index must within a 32 KiB boundary not
   * yet touched by the MT_StreamReader threads.
   * To register first query, call before processing any updates from the stream
   * When registering second (or later) query, call register_query after post_query_resume but 
   * before calls to get_edge() to ensure the registration and query succeed
   * @param query_idx   the stream update index directly after which the query will be performed
   * @return            true if the query is successfully registered and false if not
  */
  bool register_query(uint64_t query_idx) {
    uint64_t byte_index = header_size + query_idx * edge_size;
    if (byte_index <= stream_off) return false;
    else query_index = byte_index;
    return true;
  }

  inline void stream_reset() {stream_off = header_size;}
  inline uint32_t nodes() {return num_nodes;}
  inline uint64_t edges() {return num_edges;}
  BinaryGraphStream_MT(const BinaryGraphStream_MT &) = delete;
  BinaryGraphStream_MT & operator=(const BinaryGraphStream_MT &) = delete;
  friend class MT_StreamReader;
private:
  int stream_fd;
  uint32_t num_nodes;    // number of nodes in the graph
  uint64_t num_edges;    // number of edges in the graph stream
  uint32_t buf_size;     // how big is the data buffer
  uint64_t end_of_file;  // the index of the end of the file
  std::atomic<uint64_t> stream_off;  // where do threads read from in the stream
  std::atomic<uint64_t> query_index; // what is the index of the next query in bytes
  std::atomic<bool> query_block;     // If true block read_data calls and have thr return BREAKPOINT
  const uint32_t edge_size = sizeof(uint8_t) + 2 * sizeof(uint32_t); // size of binary encoded edge
  const size_t header_size = sizeof(node_id_t) + sizeof(edge_id_t); // size of num_nodes + num_upds

  inline uint32_t read_data(char *buf) {
    // we are blocking on a query or the stream is done so don't fetch_add or read
    if (query_block || stream_off >= end_of_file || stream_off >= query_index) return 0;

    // multiple threads may execute this line of code at once. This can cause edge cases
    uint64_t read_off = stream_off.fetch_add(buf_size, std::memory_order_relaxed);

    // we catch these edge cases using the two below checks
    if (read_off >= query_index) {
      stream_off = query_index.load();
      return 0;
    }
    if (read_off >= end_of_file) return 0;
    
    // perform read using pread and ensure amount of data read is of appropriate size
    size_t data_read = 0;
    size_t data_to_read = buf_size;
    if (query_index >= read_off && query_index < read_off + buf_size) {
      data_to_read = query_index - read_off; // query truncates the read
      stream_off   = query_index.load();
    }
    if (read_off + data_to_read > end_of_file)
      data_to_read = end_of_file - read_off; // EOF truncates the read

    while (data_read < data_to_read) {
      int ret = pread(stream_fd, buf, data_to_read, read_off + data_read); // perform the read
      if (ret == -1) throw StreamFailedException();
      data_read += ret;
    }
    return data_read;
  }
};

// this class provides an interface for interacting with the
// BinaryGraphStream_MT from a single thread
class MT_StreamReader {
public:
  MT_StreamReader(BinaryGraphStream_MT &stream) :
    stream(stream), buf((char *) malloc(stream.buf_size * sizeof(char))), start_buf(buf) {}

  ~MT_StreamReader() {free(start_buf);}

  inline GraphUpdate get_edge() {
    // if we have read all the data in the buffer than refill it
    if (buf - start_buf >= data_in_buf) {
      if ((data_in_buf = stream.read_data(start_buf)) == 0) {
        return {{0, 0}, BREAKPOINT}; // return that a break point has been reached
      }
      buf = start_buf; // point buf back to beginning of data buffer
    }

    UpdateType u = (UpdateType) *buf;
    uint32_t a;
    uint32_t b;

    std::memcpy(&a, buf + 1, sizeof(uint32_t));
    std::memcpy(&b, buf + 5, sizeof(uint32_t));
    
    buf += stream.edge_size;

    return {{a,b}, u};
  }

private:
  BinaryGraphStream_MT &stream; // stream to pull data from
  char *buf;                    // data buffer
  char *start_buf;              // the start of the data buffer
  uint32_t data_in_buf = 0;     // amount of data in data buffer
};
