/*
** analyze.h
** Lua bytecode analyzer header
*/

#ifndef lanalyze_h
#define lanalyze_h

#include <stdio.h>
#include "lobject.h"

typedef struct InterfaceReport InterfaceReport;
InterfaceReport *analyze_proto(const Proto *f);
void print_report_json(InterfaceReport *report, FILE *out);
void free_report(InterfaceReport *report);

#endif