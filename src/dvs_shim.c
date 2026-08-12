/*
** dvs_shim.c
** The swarm host vtable, for a host on the far side of a wasm boundary.
**
** 'dvs_new' takes C function pointers and a JavaScript host cannot make one:
** a wasm function pointer is an index into the module's function table, and
** installing one from outside means exporting a mutable table and reserving a
** slot -- the same reason dv.h gives for 'dv_endpoint_allow' existing beside
** the callback it replaces. So the trampolines live here, on the C side of
** the boundary, declared as imports the embedder supplies at instantiation:
** 'js_host_create', 'js_host_destroy' and 'js_host_drive', module "env",
** the pattern 'wasm_stubs_unknown.c' already uses for '_diluvium_write'.
**
** 'dvsjs_new' is the constructor a wasm host calls instead of 'dvs_new'. The
** vtable it builds is always fully populated -- an embedder that wants no
** create/destroy supplies '() => 0' / '() => {}' on the JS side, which is
** cheaper than teaching this file which imports were real. 'ud' is NULL
** throughout: a JS host keys its per-instance state by 'dvs_id', which it
** receives in every call, rather than by a pointer it cannot dereference.
**
** Compiled into the wasm targets only. A native host calls 'dvs_new' with
** real function pointers and has no business here. A native compile of this
** file only warns (the import attributes are ignored); it is the native
** *link* that fails, on three js_host_* externs nothing defines -- still the
** right outcome, just one stage later than the attributes suggest.
**
** Consequence worth stating where the embedder will read it: a module
** containing this file cannot instantiate without the three "env" imports.
** The JS binding supplies safe defaults (a throwing 'drive', so a host that
** forgot to install one learns it by name) whether or not a swarm is used.
*/

#include "dvs.h"

__attribute__((import_module("env"), import_name("js_host_create")))
extern void *js_host_create (void *ud, dvs_id id, dv_instance *inst);

__attribute__((import_module("env"), import_name("js_host_destroy")))
extern void js_host_destroy (void *ud, dvs_id id, void *ctx);

__attribute__((import_module("env"), import_name("js_host_drive")))
extern int js_host_drive (void *ud, dvs_id id, dv_instance *inst, void *ctx);

__attribute__((export_name("dvsjs_new")))
dvs_swarm *dvsjs_new (uint32_t max_instances, uint32_t spawns_per_step) {
  dvs_host h;
  h.create = js_host_create;
  h.destroy = js_host_destroy;
  h.drive = js_host_drive;
  h.ud = (void *)0;
  return dvs_new(&h, max_instances, spawns_per_step);
}
