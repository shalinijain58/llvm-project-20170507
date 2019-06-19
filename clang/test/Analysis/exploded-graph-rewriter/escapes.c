// FIXME: Figure out how to use %clang_analyze_cc1 with our lit.local.cfg.
// RUN: %clang_cc1 -analyze -triple x86_64-unknown-linux-gnu \
// RUN:                     -analyzer-checker=core \
// RUN:                     -analyzer-dump-egraph=%t.dot %s
// RUN: %exploded_graph_rewriter %t.dot | FileCheck %s

// FIXME: Substitution doesn't seem to work on Windows.
// UNSUPPORTED: system-windows

void string_region_escapes() {
  // CHECK: <td align="left"><b>Store: </b></td>
  // CHECK-SAME: <td align="left">foo</td><td align="left">0</td>
  // CHECK-SAME: <td align="left">&amp;Element\{"foo",0 S64b,char\}</td>
  // CHECK: <td align="left"><b>Environment: </b></td>
  // CHECK-SAME: <td align="left">"foo"</td>
  // CHECK-SAME: <td align="left">&amp;Element\{"foo",0 S64b,char\}</td>
  const char *const foo = "foo";
}
