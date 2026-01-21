/*
** analyze.c
** Lua bytecode analyzer for luac --report flag
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define lanalyze_c
#define LUA_CORE

#include "lua.h"
#include "lobject.h"
#include "lstate.h"
#include "lundump.h"
#include "lopcodes.h"

typedef struct {
  const char *name;
  int param_count;
  int is_vararg;
  int returns_table;
  int upvalue_count;
  const char **upvalue_names;
  const char **param_names;
  int array_hint;
  int hash_hint;
  size_t estimated_bytes;
  int contains_closures;
  const char *closure_warning;
} FunctionInfo;

typedef struct {
  FunctionInfo *functions;
  int num_functions;
  int capacity_functions;
  
  const char **global_functions;
  int num_global_functions;
  int capacity_global_functions;
  
  const char **global_variables;
  int num_global_variables;
  int capacity_global_variables;
} InterfaceReport;

static InterfaceReport *create_report(void) {
  InterfaceReport *report = (InterfaceReport *)malloc(sizeof(InterfaceReport));
  report->functions = NULL;
  report->num_functions = 0;
  report->capacity_functions = 0;
  report->global_functions = NULL;
  report->num_global_functions = 0;
  report->capacity_global_functions = 0;
  report->global_variables = NULL;
  report->num_global_variables = 0;
  report->capacity_global_variables = 0;
  return report;
}

static const char *copy_string(const char *str) {
  if (!str) return NULL;
  size_t len = strlen(str);
  char *copy = (char *)malloc(len + 1);
  strcpy(copy, str);
  return copy;
}

static void add_function(InterfaceReport *report, FunctionInfo *info) {
  if (report->num_functions >= report->capacity_functions) {
    int new_capacity = report->capacity_functions == 0 ? 8 : report->capacity_functions * 2;
    report->functions = (FunctionInfo *)realloc(report->functions, 
                                                 new_capacity * sizeof(FunctionInfo));
    report->capacity_functions = new_capacity;
  }
  report->functions[report->num_functions++] = *info;
}

static void add_global_function(InterfaceReport *report, const char *name) {
  if (report->num_global_functions >= report->capacity_global_functions) {
    int new_capacity = report->capacity_global_functions == 0 ? 8 : report->capacity_global_functions * 2;
    report->global_functions = (const char **)realloc(report->global_functions,
                                                       new_capacity * sizeof(const char *));
    report->capacity_global_functions = new_capacity;
  }
  report->global_functions[report->num_global_functions++] = copy_string(name);
}

static void add_global_variable(InterfaceReport *report, const char *name) {
  if (report->num_global_variables >= report->capacity_global_variables) {
    int new_capacity = report->capacity_global_variables == 0 ? 8 : report->capacity_global_variables * 2;
    report->global_variables = (const char **)realloc(report->global_variables,
                                                       new_capacity * sizeof(const char *));
    report->capacity_global_variables = new_capacity;
  }
  report->global_variables[report->num_global_variables++] = copy_string(name);
}

/* Convert floating point byte to int (Lua's size hint encoding) */
static int fb2int(int x) {
  int e = (x >> 3) & 0x1f;
  if (e == 0) return x;
  return ((x & 7) + 8) << (e - 1);
}

static void analyze_proto_recursive(const Proto *f, InterfaceReport *report, int is_top_level);

static void analyze_function(const Proto *f, InterfaceReport *report, int is_top_level) {
  FunctionInfo info;
  memset(&info, 0, sizeof(FunctionInfo));
  
  /* Get function name */
  if (f->source && getstr(f->source)) {
    const char *src = getstr(f->source);
    if (f->linedefined > 0) {
      char buf[256];
      snprintf(buf, sizeof(buf), "%s:%d", src, f->linedefined);
      info.name = copy_string(buf);
    } else {
      info.name = copy_string(src);
    }
  } else {
    info.name = copy_string("(unknown)");
  }
  
  /* Basic function info */
  info.param_count = f->numparams;
  info.is_vararg = f->is_vararg;
  info.upvalue_count = f->sizeupvalues;
  info.returns_table = 0;
  info.contains_closures = 0;
  info.closure_warning = NULL;
  
  /* Get parameter names */
  if (f->numparams > 0) {
    info.param_names = (const char **)malloc(f->numparams * sizeof(const char *));
    for (int i = 0; i < f->numparams; i++) {
      if (i < f->sizelocvars && f->locvars[i].varname) {
        info.param_names[i] = copy_string(getstr(f->locvars[i].varname));
      } else {
        info.param_names[i] = copy_string("(unknown)");
      }
    }
  } else {
    info.param_names = NULL;
  }
  
  /* Get upvalue names */
  if (f->sizeupvalues > 0) {
    info.upvalue_names = (const char **)malloc(f->sizeupvalues * sizeof(const char *));
    for (int i = 0; i < f->sizeupvalues; i++) {
      if (f->upvalues && f->upvalues[i].name) {
        info.upvalue_names[i] = copy_string(getstr(f->upvalues[i].name));
      } else {
        info.upvalue_names[i] = copy_string("(unknown)");
      }
    }
  } else {
    info.upvalue_names = NULL;
  }
  
  /* Analyze bytecode */
  int last_newtable_pc = -1;
  int array_hint = 0;
  int hash_hint = 0;
  
  for (int pc = 0; pc < f->sizecode; pc++) {
    Instruction i = f->code[pc];
    OpCode op = GET_OPCODE(i);
    
    switch (op) {
      case OP_NEWTABLE: {
        /* Extract size hints */
        int b = GETARG_B(i);
        int c = GETARG_C(i);
        array_hint = fb2int(b);
        hash_hint = fb2int(c);
        last_newtable_pc = pc;
        break;
      }
      
      case OP_RETURN:
      case OP_RETURN0:
      case OP_RETURN1: {
        /* Check if returning a table created just before */
        if (last_newtable_pc >= 0 && pc - last_newtable_pc < 20) {
          info.returns_table = 1;
          info.array_hint = array_hint;
          info.hash_hint = hash_hint;
          /* Rough estimate: 32 bytes overhead + array + hash parts */
          info.estimated_bytes = 32 + (array_hint * 16) + (hash_hint * 32);
        }
        break;
      }
      
      case OP_CLOSURE: {
        int bx = GETARG_Bx(i);
        if (bx < f->sizep) {
          Proto *child = f->p[bx];
          if (child->sizeupvalues > 0) {
            info.contains_closures = 1;
            /* Try to get closure function name */
            if (child->linedefined > 0) {
              char buf[512];
              snprintf(buf, sizeof(buf), 
                      "Closure defined at line %d creates new closure per call",
                      child->linedefined);
              info.closure_warning = copy_string(buf);
            }
          }
        }
        break;
      }
      
      case OP_SETTABUP: {
        /* Check if setting global */
        if (GETARG_A(i) == 0) {  /* _ENV */
          int key_idx = GETARG_B(i);
          if (key_idx < f->sizek) {
            TValue *k = &f->k[key_idx];
            if (ttisstring(k)) {
              const char *name = getstr(tsvalue(k));
              
              /* Check next instruction to see if it's a function */
              if (pc + 1 < f->sizecode) {
                Instruction next = f->code[pc + 1];
                OpCode next_op = GET_OPCODE(next);
                if (next_op == OP_CLOSURE) {
                  add_global_function(report, name);
                } else {
                  add_global_variable(report, name);
                }
              } else {
                add_global_variable(report, name);
              }
            }
          }
        }
        break;
      }
    }
  }
  
  add_function(report, &info);
  
  /* Recursively analyze nested functions */
  for (int i = 0; i < f->sizep; i++) {
    analyze_proto_recursive(f->p[i], report, 0);
  }
}

static void analyze_proto_recursive(const Proto *f, InterfaceReport *report, int is_top_level) {
  analyze_function(f, report, is_top_level);
}

InterfaceReport *analyze_proto(const Proto *f) {
  InterfaceReport *report = create_report();
  analyze_proto_recursive(f, report, 1);
  return report;
}

void print_report_json(InterfaceReport *report, FILE *out) {
  fprintf(out, "{\n");
  fprintf(out, "  \"functions\": [\n");
  
  for (int i = 0; i < report->num_functions; i++) {
    FunctionInfo *f = &report->functions[i];
    fprintf(out, "    {\n");
    fprintf(out, "      \"name\": \"%s\",\n", f->name ? f->name : "unknown");
    fprintf(out, "      \"params\": %d,\n", f->param_count);
    
    if (f->param_count > 0 && f->param_names) {
      fprintf(out, "      \"param_names\": [");
      for (int j = 0; j < f->param_count; j++) {
        fprintf(out, "\"%s\"", f->param_names[j]);
        if (j < f->param_count - 1) fprintf(out, ", ");
      }
      fprintf(out, "],\n");
    }
    
    fprintf(out, "      \"vararg\": %s,\n", f->is_vararg ? "true" : "false");
    
    if (f->upvalue_count > 0 && f->upvalue_names) {
      fprintf(out, "      \"upvalues\": [");
      for (int j = 0; j < f->upvalue_count; j++) {
        fprintf(out, "\"%s\"", f->upvalue_names[j]);
        if (j < f->upvalue_count - 1) fprintf(out, ", ");
      }
      fprintf(out, "],\n");
    } else {
      fprintf(out, "      \"upvalues\": [],\n");
    }
    
    if (f->returns_table) {
      fprintf(out, "      \"returns_table\": true,\n");
      fprintf(out, "      \"table_info\": {\n");
      fprintf(out, "        \"array_hint\": %d,\n", f->array_hint);
      fprintf(out, "        \"hash_hint\": %d,\n", f->hash_hint);
      fprintf(out, "        \"estimated_bytes\": %zu", f->estimated_bytes);
      
      if (f->contains_closures) {
        fprintf(out, ",\n        \"contains_closures\": true");
        if (f->closure_warning) {
          fprintf(out, ",\n        \"closure_warning\": \"%s\"", f->closure_warning);
        }
      }
      
      fprintf(out, "\n      }\n");
    } else {
      fprintf(out, "      \"returns_table\": false\n");
    }
    
    fprintf(out, "    }");
    if (i < report->num_functions - 1) fprintf(out, ",");
    fprintf(out, "\n");
  }
  
  fprintf(out, "  ],\n");
  fprintf(out, "  \"globals_after_load\": {\n");
  fprintf(out, "    \"functions\": [");
  
  for (int i = 0; i < report->num_global_functions; i++) {
    fprintf(out, "\"%s\"", report->global_functions[i]);
    if (i < report->num_global_functions - 1) fprintf(out, ", ");
  }
  
  fprintf(out, "],\n");
  fprintf(out, "    \"variables\": [");
  
  for (int i = 0; i < report->num_global_variables; i++) {
    fprintf(out, "\"%s\"", report->global_variables[i]);
    if (i < report->num_global_variables - 1) fprintf(out, ", ");
  }
  
  fprintf(out, "]\n");
  fprintf(out, "  }\n");
  fprintf(out, "}\n");
}

void free_report(InterfaceReport *report) {
  if (!report) return;
  
  for (int i = 0; i < report->num_functions; i++) {
    FunctionInfo *f = &report->functions[i];
    if (f->name) free((void *)f->name);
    if (f->param_names) {
      for (int j = 0; j < f->param_count; j++) {
        if (f->param_names[j]) free((void *)f->param_names[j]);
      }
      free((void *)f->param_names);
    }
    if (f->upvalue_names) {
      for (int j = 0; j < f->upvalue_count; j++) {
        if (f->upvalue_names[j]) free((void *)f->upvalue_names[j]);
      }
      free((void *)f->upvalue_names);
    }
    if (f->closure_warning) free((void *)f->closure_warning);
  }
  
  if (report->functions) free(report->functions);
  
  for (int i = 0; i < report->num_global_functions; i++) {
    if (report->global_functions[i]) free((void *)report->global_functions[i]);
  }
  if (report->global_functions) free(report->global_functions);
  
  for (int i = 0; i < report->num_global_variables; i++) {
    if (report->global_variables[i]) free((void *)report->global_variables[i]);
  }
  if (report->global_variables) free(report->global_variables);
  
  free(report);
}