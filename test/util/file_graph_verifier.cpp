#include "../../include/test/file_graph_verifier.h"

#include <map>
#include <iostream>
#include <algorithm>
#include <cassert>

FileGraphVerifier::FileGraphVerifier(node_id_t n, const std::string &input_file) : sets(n) {
  std::ifstream in(input_file);
  if (!in) {
    throw std::invalid_argument("FileGraphVerifier: Could not open: " + input_file);
  }

  kruskal_ref = kruskal(input_file);
  node_id_t num_nodes;
  edge_id_t m;
  node_id_t a, b;
  in >> num_nodes >> m;
  if (num_nodes != n) throw std::invalid_argument("num_nodes != n in FileGraphVerifier");

  for (unsigned i = 0; i < n; ++i) {
    boruvka_cc.push_back({i});
    adj_matrix.emplace_back(n - i);
  }
  while (m--) {
    in >> a >> b;
    if (a > b) std::swap(a, b);
    b = b - a;
    adj_matrix[a][b] = !adj_matrix[a][b];
  }
  in.close();
}

std::vector<std::set<node_id_t>> FileGraphVerifier::kruskal(const std::string& input_file) {
  std::ifstream in(input_file);
  node_id_t n;
  edge_id_t m;
  in >> n >> m;
  DisjointSetUnion<node_id_t> kruskal_sets(n);
  int a,b;
  while (m--) {
    in >> a >> b;
    kruskal_sets.merge(a,b);
  }
  in.close();

  std::map<node_id_t, std::set<node_id_t>> temp;
  for (unsigned i = 0; i < n; ++i) {
    temp[kruskal_sets.find_root(i)].insert(i);
  }

  std::vector<std::set<node_id_t>> retval;
  retval.reserve(temp.size());
  for (const auto& entry : temp) {
    retval.push_back(entry.second);
  }
  return retval;
}

void FileGraphVerifier::verify_edge(Edge edge) {
  if (edge.src > edge.dst) std::swap(edge.src, edge.dst);
  if (!adj_matrix[edge.src][edge.dst - edge.src]) {
    printf("Got an error on edge (%u, %u): edge is not in graph!\n", edge.src, edge.dst);
    throw BadEdgeException();
  }

  DSUMergeRet<node_id_t> ret = sets.merge(edge.src, edge.dst);
  if (!ret.merged) {
    printf("Got an error of node (%u, %u): components already joined!\n", edge.src, edge.dst);
    throw BadEdgeException();
  }

  // if all checks pass, merge supernodes
  for (auto& i : boruvka_cc[ret.child]) boruvka_cc[ret.root].insert(i);
}

void FileGraphVerifier::verify_cc(node_id_t node) {
  node = sets.find_root(node);
  for (const auto& cc : kruskal_ref) {
    if (boruvka_cc[node] == cc) return;
  }
  throw NotCCException();
}

void FileGraphVerifier::verify_soln(std::vector<std::set<node_id_t>> &retval) {
  auto temp {retval};
  std::sort(temp.begin(),temp.end());
  std::sort(kruskal_ref.begin(),kruskal_ref.end());
  if (kruskal_ref != temp)
    throw IncorrectCCException();

  std::cout << "Solution ok: " << retval.size() << " CCs found." << std::endl;
}
