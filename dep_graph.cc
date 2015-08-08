#include <iostream>

#include "util.h"
#include "expr_functions.h"
#include "graph.h"
#include "clang_utility_functions.h"
#include "pkt_func_transform.h"
#include "single_pass.h"

using namespace clang;

/// Identify statements that read from and write to state
/// And create a back edge from the write back to the read.
/// This is really the crux of the compiler:
/// back edges from stateful writes to the next stateful read.
static Graph<const BinaryOperator *> handle_state_vars(const std::vector<const BinaryOperator *> & stmt_vector, const Graph<const BinaryOperator*> & dep_graph) {
  Graph<const BinaryOperator*> ret = dep_graph;
  std::map<std::string, const BinaryOperator *> state_reads;
  std::map<std::string, const BinaryOperator *> state_writes;
  for (const auto * stmt : stmt_vector) {
    const auto * lhs = stmt->getLHS()->IgnoreParenImpCasts();
    const auto * rhs = stmt->getRHS()->IgnoreParenImpCasts();
    if (isa<DeclRefExpr>(rhs)) {
      state_reads[clang_stmt_printer(rhs)] = stmt;
    } else if (isa<DeclRefExpr>(lhs)) {
      state_writes[clang_stmt_printer(lhs)] = stmt;
      const auto state_var = clang_stmt_printer(lhs);
      ret.add_edge(state_reads.at(state_var), state_writes.at(state_var));
      ret.add_edge(state_writes.at(state_var), state_reads.at(state_var));
    }
  }
  return ret;
}

/// Does a particular operation read a variable
static bool op_reads_var(const BinaryOperator * op, const Expr * var) {
  assert(op);
  assert(var);

  // All reads happen only on the RHS
  const auto read_vars = ExprFunctions::get_vars(op->getRHS());

  return (std::find(read_vars.begin(), read_vars.end(), clang_stmt_printer(var)) != read_vars.end());
}

/// Is there a dependence from op1 to op2?
/// Requiring op1 to be executed before op2?
static bool depends(const BinaryOperator * op1, const BinaryOperator * op2) {
  // If op1 succeeds op2 in program order,
  // return false right away
  if (not (op1->getLocStart() < op2->getLocStart())) {
    return false;
  }

  // op1 writes the same variable that op2 writes (Write After Write)
  if (clang_stmt_printer(op1->getLHS()) == clang_stmt_printer(op2->getLHS())) {
    throw std::logic_error("Cannot have Write-After-Write dependencies in SSA form from " + clang_stmt_printer(op1) + " to " + clang_stmt_printer(op2) + "\n");
  }

  // op1 reads a variable that op2 writes (Write After Read)
  if (op_reads_var(op1, op2->getLHS())) {
    // Make an exception for state variables. There is no way around this.
    // There is no need to add this edge, because handle_state_vars() does
    // this already.
    if (isa<DeclRefExpr>(op2->getLHS())) {
      return false;
    } else {
      throw std::logic_error("Cannot have Write-After-Read dependencies in SSA form from " + clang_stmt_printer(op1) +  " to " + clang_stmt_printer(op2) + "\n");
    }
  }

  // op1 writes a variable (LHS) that op2 reads. (Read After Write)
  return (op_reads_var(op2, op1->getLHS()));
}

/// Is there a dependence from scc1 to scc2 (because of their constituent operations?)
static bool scc_depends(const std::vector<const BinaryOperator*> & scc1, const std::vector<const BinaryOperator*> & scc2) {
  for (const auto & op1 : scc1) {
    for (const auto & op2 : scc2) {
      if (depends(op1, op2)) return true;
    }
  }
  return false;
}

/// Print out dependency graph
/// And condensed graph onces Stongly Connected Components
/// have been condensed together
static std::pair<std::string, std::vector<std::string>> dep_graph_transform(const CompoundStmt * function_body, const std::string & pkt_name __attribute__ ((unused))) {
  // Newly created packet temporaries
  std::vector<std::string> new_decls = {};

  // Verify that it's in SSA
  // and append to a vector of const BinaryOperator *
  // in order of statement occurence.
  std::set<std::string> assigned_vars;
  std::vector<const BinaryOperator *> stmt_vector;
  for (const auto * child : function_body->children()) {
    assert(isa<BinaryOperator>(child));
    const auto * bin_op = dyn_cast<BinaryOperator>(child);
    assert(bin_op->isAssignmentOp());
    const auto * lhs = bin_op->getLHS()->IgnoreParenImpCasts();
    const auto pair = assigned_vars.emplace(clang_stmt_printer(lhs));
    if (pair.second == false) {
      throw std::logic_error("Program not in SSA form\n");
    }
    stmt_vector.emplace_back(bin_op);
  }

  // Dependency graph creation
  Graph<const BinaryOperator *> dep_graph(clang_stmt_printer);
  for (const auto * stmt : stmt_vector) {
    dep_graph.add_node(stmt);
  }

  // Handle state variables specially
  dep_graph = handle_state_vars(stmt_vector, dep_graph);

  // Now add all Read After Write Dependencies, comparing a statement only with
  // a successor statement
  for (uint32_t i = 0; i < stmt_vector.size(); i++) {
    for (uint32_t j = i + 1; j < stmt_vector.size(); j++) {
      if (depends(stmt_vector.at(i), stmt_vector.at(j))) {
        dep_graph.add_edge(stmt_vector.at(i), stmt_vector.at(j));
      }
    }
  }
  std::cerr << dep_graph << std::endl;

  // Extract sccs
  auto sccs = dep_graph.scc();
  for (auto & scc : sccs) {
    // Put statements within an SCC in program order
    std::sort(scc.begin(), scc.end(), [] (const auto * op1, const auto * op2) {return op1->getLocStart() < op2->getLocStart();});
  }

  // Graph condensation: Add SCCs as nodes
  Graph<std::vector<const BinaryOperator*>> condensed_graph(
      [] (const auto & x)
      { std::string ret = "";
        for (auto & op : x) {
          ret += clang_stmt_printer(op) + ";\n";
        }
        return ret;
      });

  for (uint32_t i = 0; i < sccs.size(); i++) {
    condensed_graph.add_node(sccs.at(i));
  }

  // Graph condensation: Add edges between SCCs
  for (uint32_t i = 0; i < sccs.size(); i++) {
    for (uint32_t j = 0; j < sccs.size(); j++) {
      if (scc_depends(sccs.at(i), sccs.at(j)) and (i != j)) {
        condensed_graph.add_edge(sccs.at(i), sccs.at(j));
      }
    }
  }

  std::cerr << condensed_graph << std::endl;

  return std::make_pair(clang_stmt_printer(function_body), new_decls);
}

static std::string help_string(""
"Print out dependency graph of the program as a dot file");

int main(int argc, const char ** argv) {
  // Parse file once and output dot file
  std::cout << SinglePass<std::string>(get_file_name(argc, argv, help_string), help_string, std::bind(pkt_func_transform, std::placeholders::_1, dep_graph_transform)).output();
}
