#include <algorithm>
#include <set>
#include <utility>

#include "array_conv.h"
#include <ansi-c/c_types.h>

static inline bool
array_indexes_are_same(
    const array_convt::idx_record_containert &a,
    const array_convt::idx_record_containert &b)
{
  if (a.size() != b.size())
    return false;

  for (const auto &e : a) {
    if (b.find(e.idx) == b.end())
      return false;
  }

  return true;
}

array_convt::array_convt(smt_convt *_ctx) : array_iface(true, true),
  array_indexes(), array_values(), array_updates(), ctx(_ctx)
{
}

array_convt::~array_convt()
{
}

void
array_convt::convert_array_assign(const array_ast *src, smt_astt sym)
{

  // Implement array assignments by simply making the destination AST track the
  // same array. No new variables need be introduced, saving lots of searching
  // hopefully. This works because we're working with an SSA program where the
  // source array will never be modified.

  // Get a mutable reference to the destination
  array_ast *destination = const_cast<array_ast*>(array_downcast(sym));
  const array_ast *source = src;

  // And copy across it's valuation
  destination->array_fields = source->array_fields;
  destination->base_array_id = source->base_array_id;
  destination->array_update_num = source->array_update_num;
  return;
}

unsigned int
array_convt::new_array_id(void)
{
  unsigned int new_base_array_id = array_indexes.size();

  // Pouplate tracking data with empt containers
  idx_record_containert tmp_set;
  array_indexes.push_back(tmp_set);

  std::vector<std::list<struct array_select> > tmp2;
  array_values.push_back(tmp2);

  std::list<struct array_select> tmp25;
  array_values[new_base_array_id].push_back(tmp25);

  std::vector<struct array_with> tmp3;
  array_updates.push_back(tmp3);

  // Aimless piece of data, just to keep indexes in iarray_updates and
  // array_values in sync.
  struct array_with w;
  w.is_ite = false;
  w.idx = expr2tc();
  array_updates[new_base_array_id].push_back(w);

  return new_base_array_id;
}

smt_ast *
array_convt::mk_array_symbol(const std::string &name, smt_sortt ms,
                             smt_sortt subtype)
{
  assert(subtype->id != SMT_SORT_ARRAY && "Can't create array of arrays with "
         "array flattener. Should be flattened elsewhere");

  // Create either a new bounded or unbounded array.
  unsigned long domain_width = ms->domain_width;
  unsigned long array_size = 1UL << domain_width;

  // Create new AST storage
  array_ast *mast = new_ast(ms);
  mast->symname = name;

  if (is_unbounded_array(mast->sort)) {
    // Don't attempt to initialize: this array is of unbounded size. Instead,
    // record a fresh new array.

    // Array ID: identifies an array at a level that corresponds to 'level1'
    // renaming, or having storage in C. Accumulates a history of selects and
    // updates.
    mast->base_array_id = new_array_id();
    mast->array_update_num = 0;

    array_subtypes.push_back(subtype);

    return mast;
  }

  // For bounded arrays, populate it's storage vector with a bunch of fresh bvs
  // of the correct sort.
  mast->array_fields.reserve(array_size);

  unsigned long i;
  for (i = 0; i < array_size; i++) {
    smt_astt a = ctx->mk_fresh(subtype, "array_fresh_array::");
    mast->array_fields.push_back(a);
  }

  return mast;
}

smt_astt 
array_convt::mk_select(const array_ast *ma, const expr2tc &idx,
                         smt_sortt ressort)
{

  // Create a select: either hand off to the unbounded implementation, or
  // continue for bounded-size arrays
  if (is_unbounded_array(ma->sort))
    return mk_unbounded_select(ma, idx, ressort);

  assert(ma->array_fields.size() != 0);

  // If this is a constant index, then simply access the designated element.
  if (is_constant_int2t(idx)) {
    const constant_int2t &intref = to_constant_int2t(idx);
    unsigned long intval = intref.constant_value.to_ulong();
    if (intval > ma->array_fields.size())
      // Return a fresh value.
      return ctx->mk_fresh(ressort, "array_mk_select_badidx::");

    // Otherwise,
    return ma->array_fields[intval];
  }

  // For undetermined indexes, create a large case switch across all values.
  smt_astt fresh = ctx->mk_fresh(ressort, "array_mk_select::");
  smt_astt real_idx = ctx->convert_ast(idx);
  unsigned long dom_width = ma->sort->domain_width;
  smt_sortt bool_sort = ctx->boolean_sort;

  for (unsigned long i = 0; i < ma->array_fields.size(); i++) {
    smt_astt tmp_idx = ctx->mk_smt_bvint(BigInt(i), false, dom_width);
    smt_astt idx_eq = real_idx->eq(ctx, tmp_idx);
    smt_astt val_eq = fresh->eq(ctx, ma->array_fields[i]);

    ctx->assert_ast(ctx->mk_func_app(bool_sort, SMT_FUNC_IMPLIES,
                                     idx_eq, val_eq));
  }

  return fresh;
}

smt_astt 
array_convt::mk_store(const array_ast* ma, const expr2tc &idx,
                                smt_astt value, smt_sortt ressort)
{

  // Create a store: initially, consider whether to hand off to the unbounded
  // implementation.
  if (is_unbounded_array(ma->sort))
    return mk_unbounded_store(ma, idx, value, ressort);

  assert(ma->array_fields.size() != 0);

  array_ast *mast = new_ast(ressort, ma->array_fields);

  // If this is a constant index, simply update that particular field.
  if (is_constant_int2t(idx)) {
    const constant_int2t &intref = to_constant_int2t(idx);
    unsigned long intval = intref.constant_value.to_ulong();
    if (intval > ma->array_fields.size())
      return ma;

    // Otherwise,
    mast->array_fields[intval] = value;
    return mast;
  }

  // For undetermined indexes, conditionally update each element of the bounded
  // array.
  smt_astt real_idx = ctx->convert_ast(idx);
  smt_astt real_value = value;
  unsigned long dom_width = mast->sort->domain_width;

  for (unsigned long i = 0; i < mast->array_fields.size(); i++) {
    smt_astt this_idx = ctx->mk_smt_bvint(BigInt(i), false, dom_width);
    smt_astt idx_eq = real_idx->eq(ctx, this_idx);

    smt_astt new_val = real_value->ite(ctx, idx_eq, mast->array_fields[i]);
    mast->array_fields[i] = new_val;
  }

  return mast;
}

smt_astt 
array_convt::mk_unbounded_select(const array_ast *ma,
                                   const expr2tc &real_idx,
                                   smt_sortt ressort)
{
  // Store everything about this select, and return a free variable, that then
  // gets constrained at the end of conversion to tie up with the correct
  // value.

  // Record that we've accessed this index.
  idx_record new_idx_rec = { real_idx, ctx->ctx_level };
  array_indexes[ma->base_array_id].insert(new_idx_rec);

  // Corner case: if the idx we're selecting is the last one updated, just
  // use that piece of AST. This simplifies things later.
  const array_with &w = array_updates[ma->base_array_id][ma->array_update_num];
  if (ma->array_update_num != 0 && !w.is_ite){
    if (real_idx == w.idx)
      return w.u.w.val;
  }

  // If the index has /already/ been selected for this particular array ast,
  // then we should return the fresh variable representing that select,
  // rather than adding another one.
  // XXX: this is a list/vec. Bad.
  for (const auto &sel : array_values[ma->base_array_id][ma->array_update_num]){
    if (sel.idx == real_idx) {
      // Aha.
      return sel.val;
    }
  }

  // Generate a new free variable
  smt_astt a = ctx->mk_fresh(ressort, "mk_unbounded_select");

  struct array_select sel;
  sel.src_array_update_num = ma->array_update_num;
  sel.idx = real_idx;
  sel.val = a;
  // Record this index
  array_values[ma->base_array_id][ma->array_update_num].push_back(sel);

  // Convert index; it might trigger an array_of, or something else, which
  // fiddles with other arrays.
  ctx->convert_ast(real_idx);

  return a;
}

smt_astt 
array_convt::mk_unbounded_store(const array_ast *ma,
                                  const expr2tc &idx, smt_astt value,
                                  smt_sortt ressort)
{
  // Store everything about this store, and suitably adjust all fields in the
  // array at the end of conversion so that they're all consistent.

  // Record that we've accessed this index.
  idx_record new_idx_rec = { idx, ctx->ctx_level };
  array_indexes[ma->base_array_id].insert(new_idx_rec);

  // More nuanced: allocate a new array representation.
  array_ast *newarr = new_ast(ressort);
  newarr->base_array_id = ma->base_array_id;
  newarr->array_update_num = array_updates[ma->base_array_id].size();

  // Record update
  struct array_with w;
  w.is_ite = false;
  w.idx = idx;
  w.u.w.src_array_update_num = ma->array_update_num;
  w.u.w.val = value;
  array_updates[ma->base_array_id].push_back(w);

  // Convert index; it might trigger an array_of, or something else, which
  // fiddles with other arrays.
  ctx->convert_ast(idx);

  // Also file a new select record for this point in time.
  std::list<struct array_select> tmp;
  array_values[ma->base_array_id].push_back(tmp);

  // Result is the new array id goo.
  return newarr;
}

smt_astt
array_convt::array_ite(smt_astt cond,
                         const array_ast *true_arr,
                         const array_ast *false_arr,
                         smt_sortt thesort)
{

  // As ever, switch between ite's of unbounded arrays or bounded ones.
  if (is_unbounded_array(true_arr->sort))
    return unbounded_array_ite(cond, true_arr, false_arr, thesort);

  // For each element, make an ite.
  assert(true_arr->array_fields.size() != 0 &&
         true_arr->array_fields.size() == false_arr->array_fields.size());
  array_ast *mast = new_ast(thesort);
  unsigned long i;
  for (i = 0; i < true_arr->array_fields.size(); i++) {
    // One ite pls.
    smt_astt res = true_arr->array_fields[i]->ite(ctx, cond,
                                                  false_arr->array_fields[i]);
    mast->array_fields.push_back(array_downcast(res));
  }

  return mast;
}

smt_astt
array_convt::unbounded_array_ite(smt_astt cond,
                                   const array_ast *true_arr,
                                   const array_ast *false_arr,
                                   smt_sortt thesort)
{
  // We can perform ite's between distinct array id's, however the precondition
  // is that they must share the same set of array indexes, otherwise there's
  // the potential for data loss.

  unsigned int new_arr_id =
    std::min(true_arr->base_array_id, false_arr->base_array_id); // yolo

  array_ast *newarr = new_ast(thesort);
  newarr->base_array_id = new_arr_id;
  newarr->array_update_num = array_updates[true_arr->base_array_id].size();

  struct array_with w;
  w.is_ite = true;
  w.idx = expr2tc();
  w.u.i.true_arr_ast = true_arr;
  w.u.i.false_arr_ast = false_arr;
  w.u.i.cond = cond;
  array_updates[new_arr_id].push_back(w);

  // Also file a new select record for this point in time.
  std::list<struct array_select> tmp;
  array_values[new_arr_id].push_back(tmp);

  return newarr;
}

smt_astt 
array_convt::convert_array_of(smt_astt init_val, unsigned long domain_width)
{
  // Create a new array, initialized with init_val
  smt_sortt dom_sort = ctx->mk_int_bv_sort(domain_width);
  smt_sortt idx_sort = init_val->sort;

  smt_sortt arr_sort = ctx->mk_sort(SMT_SORT_ARRAY, dom_sort, idx_sort);
  return convert_array_of_wsort(init_val, domain_width, arr_sort);
}

smt_astt
array_convt::convert_array_of_wsort(smt_astt init_val,
    unsigned long domain_width, smt_sortt arr_sort)
{
  smt_sortt idx_sort = init_val->sort;
  array_ast *mast = new_ast(arr_sort);

  if (is_unbounded_array(arr_sort)) {
    // If this is an unbounded array, simply store the value of the initializer
    // and constraint values at a later date. Heavy lifting is performed by
    // mk_array_symbol.
    std::string name = ctx->mk_fresh_name("array_of_unbounded::");
    mast = static_cast<array_ast*>(mk_array_symbol(name, arr_sort, idx_sort));
    array_of_vals.insert(std::make_pair(mast->base_array_id, init_val));
  } else {
    // For bounded arrays, simply store the initializer in the explicit vector
    // of elements, x times.
    unsigned long array_size = 1UL << domain_width;
    for (unsigned long i = 0; i < array_size; i++)
      mast->array_fields.push_back(init_val);
  }

  return mast;
}

smt_astt
array_convt::encode_array_equality(const array_ast *a1, const array_ast *a2)
{
  // Record an equality between two arrays at this point in time. To be
  // implemented at constraint time.

  struct array_equality e;
  e.arr1_id = a1->base_array_id;
  e.arr2_id = a2->base_array_id;
  e.arr1_update_num = a1->array_update_num;
  e.arr2_update_num = a2->array_update_num;

  e.result = ctx->mk_fresh(ctx->boolean_sort, "");

  array_equalities.push_back(e);
  return e.result;
}

smt_astt
array_convt::mk_bounded_array_equality(const array_ast *a1, const array_ast *a2)
{
  assert(a1->array_fields.size() == a2->array_fields.size());

  smt_convt::ast_vec eqs;
  for (unsigned int i = 0; i < a1->array_fields.size(); i++) {
    eqs.push_back(a1->array_fields[i]->eq(ctx, a2->array_fields[i]));
  }

  return ctx->make_conjunct(eqs);
}

expr2tc
array_convt::get_array_elem(smt_astt a, uint64_t index, const type2tc &subtype)
{
  // During model building: get the value of an array at a particular, explicit,
  // index.
  const array_ast *mast = array_downcast(a);

  if (mast->base_array_id >= array_valuation.size()) {
    // This is an array that was not previously converted, therefore doesn't
    // appear in the valuation table. Therefore, all its values are free.
    return expr2tc();
  }

  // Fetch all the indexes
  const idx_record_containert &indexes = array_indexes[mast->base_array_id];
  unsigned int i = 0;

  // Basically, we have to do a linear search of all the indexes to find one
  // that matches the index argument.
  idx_record_containert::const_iterator it;
  for (it = indexes.begin(); it != indexes.end(); it++, i++) {
    const expr2tc &e = it->idx;
    expr2tc e2 = ctx->get(e);
    if (is_nil_expr(e2))
      continue;

    const constant_int2t &intval = to_constant_int2t(e2);
    if (intval.constant_value.to_uint64() == index)
      break;
  }

  if (it == indexes.end())
    // Then this index wasn't modelled in any way.
    return expr2tc();

  // We've found an index; pick its value out, convert back to expr.

  const ast_vect &solver_values =
    array_valuation[mast->base_array_id][mast->array_update_num];
  assert(i < solver_values.size());
  return ctx->get_bv(subtype, solver_values[i]);
}

void
array_convt::add_array_constraints_for_solving(void)
{

  join_array_indexes();

  // Add constraints for each array with unique storage.
  for (unsigned int i = 0; i < array_indexes.size(); i++) {
    add_array_constraints(i);
  }

  add_array_equalities();

  return;
}

void
array_convt::join_array_indexes()
{
  // Identify the set of array ID's that, due to equalities and ITE's, are
  // effectively joined into the same array. For each of these sets, join their
  // indexes.
  // This needs to support transitivity.

  std::vector<std::set<unsigned int> > groupings;
  groupings.resize(array_updates.size());

  // Collect together the set of array id's touched by each array id.
  unsigned int arrid = 0;
  for (unsigned int arrid = 0; arrid < array_updates.size(); arrid++) {
    std::set<unsigned int> &joined_array_ids = groupings[arrid];

    for (const auto &update : array_updates[arrid]) {
      if (update.is_ite) {
        if (update.u.i.true_arr_ast->base_array_id !=
            update.u.i.false_arr_ast->base_array_id) {
          joined_array_ids.insert(update.u.i.true_arr_ast->base_array_id);
          joined_array_ids.insert(update.u.i.false_arr_ast->base_array_id);
        }
      }
    }

    joined_array_ids.insert(arrid);
  }

  for (const auto &equality : array_equalities) {
    groupings[equality.arr1_id].insert(equality.arr2_id);
    groupings[equality.arr2_id].insert(equality.arr1_id);
  }

  // K; now compute a fixedpoint joining the sets of things that touch each
  // other.
  bool modified = false;
  do {
    modified = false;

    for (const auto &arrset : groupings) {
      for (auto touched_arr_id : arrset) {
        // It the other array recorded as touching all the arrays that this one
        // does? Try inserting this set, and see if the size changes. Slightly
        // ghetto, but avoids additional allocations.
        unsigned int original_size = groupings[touched_arr_id].size();

        groupings[touched_arr_id].insert(arrset.begin(), arrset.end());

        if (original_size != groupings[touched_arr_id].size())
          modified = true;
      }
    }
  } while (modified);

  // Right -- now join all ther indexes. This can be optimised, but not now.
  for (arrid = 0; arrid < array_updates.size(); arrid++) {
    const std::set<unsigned int> &arrset = groupings[arrid];
    for (auto touched_arr_id : arrset) {
      array_indexes[arrid].insert(
          array_indexes[touched_arr_id].begin(),
          array_indexes[touched_arr_id].end());
    }
  }

  // Le fin
  return;
}

void
array_convt::add_array_equalities(void)
{
  // Precondition: all constraints have already been added and constrained into
  // the array_valuation vectors. Also that the array ID's being used all share
  // the same indexes.

  for (const auto &eq : array_equalities) {
    assert(array_indexes_are_same(array_indexes[eq.arr1_id],
                                  array_indexes[eq.arr2_id]));

    // Simply get a handle on two vectors of valuations in array_valuation,
    // and encode an equality.
    const ast_vect &a1 = array_valuation[eq.arr1_id][eq.arr1_update_num];
    const ast_vect &a2 = array_valuation[eq.arr2_id][eq.arr2_update_num];

    smt_convt::ast_vec lits;
    for (unsigned int i = 0; i < a1.size(); i++) {
      lits.push_back(a1[i]->eq(ctx, a2[i]));
    }

    smt_astt result = ctx->make_conjunct(lits);
    ctx->assert_ast(eq.result->eq(ctx, result));
  }
}

void
array_convt::add_array_constraints(unsigned int arr)
{
  // So: the plan here is that each array id has a record in 'array_valuation'.
  // Within that is a vector with an element for each array update. Each of
  // those elements is a vector again, its values being one smt_astt for each
  // possible value of the array, at a particular expr index.
  // This function builds up these vectors of values incrementally, from an
  // initial state per array id, through each array update, tying values to
  // selected elements.
  const idx_record_containert &indexes = array_indexes[arr];

  // Add a new vector for a new array.
  array_valuation.resize(array_valuation.size() + 1);
  array_update_vect &real_array_values = array_valuation.back();

  // Subtype is thus
  smt_sortt subtype = array_subtypes[arr];

  // Pre-allocate all the storage, for however many updates there are, for
  // however many array indexes there are.
  real_array_values.resize(array_values[arr].size());
  for (unsigned int i = 0; i < real_array_values.size(); i++)
    real_array_values[i].resize(indexes.size());

  // Compute a mapping between indexes and an element in the vector. These
  // are ordered by how std::set orders them, not by history or anything. Or
  // even the element index.
  idx_mapt idx_map;
  for (auto it = indexes.begin(); it != indexes.end(); it++)
    idx_map.insert(std::make_pair(it->idx, idx_map.size()));

  assert(idx_map.size() == indexes.size());

  // Initialize the first set of elements. If this array has an initializer,
  // then all values recieve the initial value. Otherwise, they receive
  // free values for each index.
  auto it = array_of_vals.find(arr);
  if (it != array_of_vals.end()) {
    collate_array_values(real_array_values[0], idx_map, array_values[arr][0],
        subtype, it->second);
  } else {
    collate_array_values(real_array_values[0], idx_map, array_values[arr][0],
        subtype);
  }

  // Ensure initial consistency of the initial values: indexes that evaluate
  // to the same concrete index should have the same value.
  add_initial_ackerman_constraints(real_array_values[0], idx_map);

  // Now repeatedly execute transitions between states.
  for (unsigned int i = 0; i < real_array_values.size() - 1; i++)
    execute_array_trans(real_array_values, arr, i, idx_map, subtype);

}

void
array_convt::execute_array_trans(array_update_vect &data,
    unsigned int arr, unsigned int idx, const idx_mapt &idx_map,
    smt_sortt subtype)
{
  // Encode the constraints for a particular array update.

  // Steps: First, fill the destination vector with either free variables, or
  // the free variables from selects corresponding to that item.
  // Then apply update or ITE constraints.
  // Then apply equalities between the old and new values.

  // The destination vector: representing the values of each element in the
  // next updated state.
  ast_vect &dest_data = data[idx+1];

  // Fill dest_data with ASTs: if a select has been applied for a particular
  // index, then that value is inserted there. Otherwise, a free value is
  // inserted.
  collate_array_values(dest_data, idx_map, array_values[arr][idx+1], subtype);

  // Two updates that could have occurred for this array: a simple with, or
  // an ite.
  const array_with &w = array_updates[arr][idx+1];
  if (w.is_ite) {
    if (w.u.i.true_arr_ast->base_array_id !=
        w.u.i.false_arr_ast->base_array_id) {
      execute_array_joining_ite(dest_data, arr, w.u.i.true_arr_ast,
                                w.u.i.false_arr_ast, idx_map, w.u.i.cond,
                                subtype);
    } else {
      unsigned int true_idx = w.u.i.true_arr_ast->array_update_num;
      unsigned int false_idx = w.u.i.false_arr_ast->array_update_num;
      assert(true_idx < idx + 1 && false_idx < idx + 1);
      execute_array_ite(dest_data, data[true_idx], data[false_idx], idx_map,
                        w.u.i.cond);
    }
  } else {
    execute_array_update(dest_data, data[w.u.w.src_array_update_num],
                         idx_map, w.idx, w.u.w.val);
  }
}

void
array_convt::execute_array_update(ast_vect &dest_data,
  ast_vect &source_data,
  const idx_mapt &idx_map,
  const expr2tc &idx,
  smt_astt updated_value)
{
  // Place a constraint on the updated variable; add equality constraints
  // between the older version and this version.

  // So, the updated element,
  idx_mapt::const_iterator it = idx_map.find(idx);
  assert(it != idx_map.end());

  smt_astt update_idx_ast = ctx->convert_ast(idx);
  unsigned int updated_idx = it->second;

  // Assign in its value. Note that no selects occur agains this data index,
  // they will have been replaced with the update ast when the select was
  // encoded.
  dest_data[updated_idx] = updated_value;

  for (auto it2 = idx_map.begin(); it2 != idx_map.end(); it2++) {
    if (it2->second == updated_idx)
      continue;

    // Generate an ITE. If the index is nondeterministically equal to the
    // current index, take the updated value, otherwise the original value.
    // This departs from the CBMC implementation, in that they explicitly
    // use implies and ackerman constraints.
    // FIXME: benchmark the two approaches. For now, this is shorter.
    smt_astt cond = update_idx_ast->eq(ctx, ctx->convert_ast(it2->first));
    smt_astt dest_ite = updated_value->ite(ctx, cond, source_data[it2->second]);
    ctx->assert_ast(dest_data[it2->second]->eq(ctx, dest_ite));
  }

  return;
}

void
array_convt::execute_array_ite(ast_vect &dest,
    const ast_vect &true_vals,
    const ast_vect &false_vals,
    const idx_mapt &idx_map,
    smt_astt cond)
{

  // Each index value becomes an ITE between each source value.
  for (unsigned int i = 0; i < idx_map.size(); i++) {
    smt_astt updated_elem = true_vals[i]->ite(ctx, cond, false_vals[i]);
    ctx->assert_ast(dest[i]->eq(ctx, updated_elem));
  }

  return;
}

void
array_convt::execute_array_joining_ite(ast_vect &dest,
    unsigned int cur_id, const array_ast *true_arr_ast,
    const array_ast *false_arr_ast, const idx_mapt &idx_map,
    smt_astt cond, smt_sortt subtype)
{

  const array_ast *local_ast, *remote_ast;
  bool local_arr_values_are_true = (true_arr_ast->base_array_id == cur_id);
  if (local_arr_values_are_true) {
    local_ast = true_arr_ast;
    remote_ast = false_arr_ast;
  } else {
    local_ast = false_arr_ast;
    remote_ast = true_arr_ast;
  }

  ast_vect selects;
  selects.reserve(array_indexes[cur_id].size());
  assert(array_indexes_are_same(array_indexes[cur_id],
                                array_indexes[remote_ast->base_array_id]));

  for (const auto &elem : array_indexes[remote_ast->base_array_id]) {
    selects.push_back(mk_unbounded_select(remote_ast, elem.idx, subtype));
  }

  // Now select which values are true or false
  const ast_vect *true_vals, *false_vals;
  if (local_arr_values_are_true) {
    true_vals =
      &array_valuation[local_ast->base_array_id][local_ast->array_update_num];
    false_vals = &selects;
  } else {
    false_vals =
      &array_valuation[local_ast->base_array_id][local_ast->array_update_num];
    true_vals = &selects;
  }

  execute_array_ite(dest, *true_vals, *false_vals, idx_map, cond);

  return;
}

void
array_convt::collate_array_values(ast_vect &vals,
                                    const idx_mapt &idx_map,
                                    const std::list<struct array_select> &idxs,
                                    smt_sortt subtype,
                                    smt_astt init_val)
{
  // IIRC, this translates the history of an array + any selects applied to it,
  // into a vector mapping a particular index to the variable representing the
  // element at that index. XXX more docs.

  // So, the value vector should be allocated but not initialized,
  assert(vals.size() == idx_map.size());

  // First, make everything null,
  for (ast_vect::iterator it = vals.begin();
       it != vals.end(); it++)
    *it = NULL;

  // Now assign in all free variables created as a result of selects.
  for (auto it = idxs.begin(); it != idxs.end(); it++) {
    auto it2 = idx_map.find(it->idx);
    assert(it2 != idx_map.end());
    vals[it2->second] = it->val;
  }

  // Initialize everything else to either a free variable or the initial value.
  if (init_val == NULL) {
    // Free variables, except where free variables tied to selects have occurred
    for (auto it = vals.begin(); it != vals.end(); it++) {
      if (*it == NULL)
        *it = ctx->mk_fresh(subtype, "collate_array_vals::");
    }
  } else {
    // We need to assign the initial value in, except where there's already
    // a select/index, in which case we assert that the values are equal.
    for (auto it = vals.begin(); it != vals.end(); it++) {
      if (*it == NULL) {
        *it = init_val;
      } else {
        ctx->assert_ast((*it)->eq(ctx, init_val));
      }
    }
  }

  // Fin.
}

void
array_convt::add_initial_ackerman_constraints(
                                  const ast_vect &vals,
                                  const idx_mapt &idx_map)
{
  // Add ackerman constraints: these state that for each element of an array,
  // where the indexes are equivalent (in the solver), then the value of the
  // elements are equivalent. The cost is quadratic, alas.

  smt_sortt boolsort = ctx->boolean_sort;
  for (auto it = idx_map.begin(); it != idx_map.end(); it++) {
    smt_astt outer_idx = ctx->convert_ast(it->first);
    for (auto it2 = idx_map.begin(); it2 != idx_map.end(); it2++) {
      smt_astt inner_idx = ctx->convert_ast(it2->first);

      // If they're the same idx, they're the same value.
      smt_astt idxeq = outer_idx->eq(ctx, inner_idx);

      smt_astt valeq = vals[it->second]->eq(ctx, vals[it2->second]);

      ctx->assert_ast(ctx->mk_func_app(boolsort, SMT_FUNC_IMPLIES,
                                       idxeq, valeq));
    }
  }
}

smt_astt
array_ast::eq(smt_convt *ctx __attribute__((unused)), smt_astt sym) const
{
  const array_ast *other = array_downcast(sym);

  if (is_unbounded_array(sort)) {
    return array_ctx->encode_array_equality(this, other);
  } else {
    return array_ctx->mk_bounded_array_equality(this, other);
  }
}

void
array_ast::assign(smt_convt *ctx __attribute__((unused)), smt_astt sym) const
{
  array_ctx->convert_array_assign(this, sym);
}

smt_astt
array_ast::update(smt_convt *ctx __attribute__((unused)), smt_astt value,
                                unsigned int idx,
                                expr2tc idx_expr) const
{
  if (is_nil_expr(idx_expr))
    idx_expr = constant_int2tc(get_uint_type(sort->domain_width), BigInt(idx));

  return array_ctx->mk_store(this, idx_expr, value, sort);
}

smt_astt
array_ast::select(smt_convt *ctx __attribute__((unused)),
                  const expr2tc &idx) const
{
  // Look up the array subtype sort. If we're unbounded, use the base array id
  // to do that, otherwise pull the subtype out of an element.
  smt_sortt s;
  if (!array_fields.empty())
    s = array_fields[0]->sort;
  else
    s = array_ctx->array_subtypes[base_array_id];

  return array_ctx->mk_select(this, idx, s);
}

smt_astt
array_ast::ite(smt_convt *ctx __attribute__((unused)),
               smt_astt cond, smt_astt falseop) const
{

  return array_ctx->array_ite(cond, this, array_downcast(falseop), sort);
}
