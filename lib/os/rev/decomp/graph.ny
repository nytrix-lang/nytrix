;; Keywords: decompiler callgraph graph edges scc
;; Pure call-graph edge traversal and strongly connected component helpers.
module std.os.rev.decomp.graph *
use std.core
use std.os.rev.decomp.collections (_append_unique)

fn _cg_edge_key(dict e) str {
   e.get("from", "") + "->" + e.get("to", "") + "@" + to_str(e.get("site", 0))
}

fn _cg_add_edge(list edges, dict edge) list {
   def k = _cg_edge_key(edge)
   mut i = 0
   while i < edges.len {
      if _cg_edge_key(edges[i]) == k { return edges }
      i += 1
   }
   edges = edges.append(edge)
   edges
}

fn _cg_successors(list edges, str name, bool internal_only=true) list {
   mut out = []
   mut i = 0
   while i < edges.len {
      def e = edges[i]
      if e.get("from", "") == name && (!internal_only || e.get("internal", false)) {
         out = _append_unique(out, e.get("to", ""))
      }
      i += 1
   }
   out
}

fn _cg_predecessors(list edges, str name, bool internal_only=true) list {
   mut out = []
   mut i = 0
   while i < edges.len {
      def e = edges[i]
      if e.get("to", "") == name && (!internal_only || e.get("internal", false)) {
         out = _append_unique(out, e.get("from", ""))
      }
      i += 1
   }
   out
}

fn _cg_reaches(str start, str goal, list edges, int limit=256) bool {
   if start == goal { return true }
   mut seen = dict().set(start, true)
   mut queue = [start]
   mut qi = 0
   while qi < queue.len && qi < limit {
      def name = queue[qi]
      qi += 1
      def successors = _cg_successors(edges, name, true)
      mut i = 0
      while i < successors.len {
         def next = successors[i]
         if next == goal { return true }
         if !seen.get(next, false) {
            seen = seen.set(next, true)
            queue = queue.append(next)
         }
         i += 1
      }
   }
   false
}

fn _cg_has_self_edge(str name, list edges) bool {
   mut i = 0
   while i < edges.len {
      if edges[i].get("internal", false) && edges[i].get("from", "") == name && edges[i].get("to", "") == name { return true }
      i += 1
   }
   false
}

fn _cg_components(list names, list edges) list {
   mut assigned = dict()
   mut components = []
   mut i = 0
   while i < names.len {
      def name = names[i]
      if !assigned.get(name, false) {
         mut component = [name]
         assigned = assigned.set(name, true)
         mut j = i + 1
         while j < names.len {
            def other = names[j]
            if !assigned.get(other, false) && _cg_reaches(name, other, edges) && _cg_reaches(other, name, edges) {
               component = component.append(other)
               assigned = assigned.set(other, true)
            }
            j += 1
         }
         components = components.append({"nodes": component, "recursive": component.len > 1 || _cg_has_self_edge(name, edges)})
      }
      i += 1
   }
   components
}

#main {
   def edges = [{"from": "a", "to": "b", "site": 1, "internal": true}, {"from": "b", "to": "a", "site": 2, "internal": true}]
   assert(_cg_components(["a", "b"], edges)[0].get("recursive", false), "recursive component")
   assert(_cg_successors(edges, "a").len == 1, "graph successor")
   print("✓ std.os.rev.decomp.graph self-test passed")
}
