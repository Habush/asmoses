/*
 * moses/moses/scoring/bscores.cc
 *
 * Copyright (C) 2002-2008 Novamente LLC
 * Copyright (C) 2012,2013 Poulin Holdings LLC
 * Copyright (C) 2014 Aidyia Limited
 * All Rights Reserved
 *
 * Written by Moshe Looks, Nil Geisweiller, Linas Vepstas
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License v3 as
 * published by the Free Software Foundation and including the exceptions
 * at http://opencog.org/wiki/Licenses
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program; if not, write to:
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include <cmath>
#include <limits.h>
#include <set>

#include <boost/range/irange.hpp>
#include <boost/range/algorithm/sort.hpp>
#include <boost/range/algorithm/for_each.hpp>
#include <boost/range/algorithm/transform.hpp>
#include <boost/range/algorithm/max_element.hpp>
#include <boost/range/algorithm_ext/for_each.hpp>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/adaptor/filtered.hpp>
#include <boost/range/adaptor/reversed.hpp>
#include <boost/range/adaptor/transformed.hpp>

#include <boost/spirit/include/phoenix_core.hpp>
#include <boost/spirit/include/phoenix_operator.hpp>

#include <opencog/util/numeric.h>
#include <opencog/util/KLD.h>
#include <opencog/util/MannWhitneyU.h>
#include <opencog/asmoses/data/table/table_io.h>
#include <opencog/asmoses/atomese/interpreter/Interpreter.h>
#include <opencog/asmoses/utils/valueUtils.h>
#include <opencog/asmoses/atomese/atomese_utils/constants.h>
#include "bscores.h"

namespace opencog
{
namespace moses
{

using namespace std;
using boost::adaptors::map_values;
using boost::adaptors::map_keys;
using boost::adaptors::filtered;
using boost::adaptors::reversed;
using boost::adaptors::transformed;
using boost::transform;
using namespace boost::phoenix;
using boost::phoenix::arg_names::arg1;
using namespace boost::accumulators;

using namespace atomese;

////////////////////
// logical_bscore //
////////////////////

behavioral_score logical_bscore::operator()(const combo_tree &tr) const
{
	combo::complete_truth_table tt(tr, _arity);
	behavioral_score bs(_size);

	// Compare the predictions of the tree to that of the desired
	// result. A correct prdiction gets a score of 0, an incorrect
	// prediction gets a score of -1.
	boost::transform(tt, _target, bs.begin(), [](bool b1, bool b2) {
		return -score_t(b1 != b2);
	});

	return bs;
}

/// Boolean ensemble scorer.  Assumes that the ensemble signature outputs
/// a boolean value.  All of the trees in the ensmble get a weighted vote,
/// that vote is totalled to get the prediction of the ensemble, and then
/// compared to the desired output.
behavioral_score logical_bscore::operator()(const scored_combo_tree_set &ensemble) const
{
	// Step 1: accumulate the weighted prediction of each tree in
	// the ensemble.
	behavioral_score hypoth(_size, 0.0);
	for (const scored_combo_tree &sct: ensemble) {
		combo::complete_truth_table tt(sct.get_tree(), _arity);
		score_t weight = sct.get_weight();
		for (size_t i = 0; i < _size; i++) {
			// Add +1 if prediction is true and -1 if prediction is false.
			// We could gain some minor performance improvement if we
			// moved this out of the loop, but who cares, this scorer is
			// used only for the demo problems.
			hypoth[i] += weight * (2.0 * ((score_t) tt[i]) - 1.0);
		}
	}

	// Step 2: compare the prediction of the ensemble to the desired
	// result. The array "hypoth" is positive to predict true, and
	// negative to predict false.  The resulting score is 0 if correct,
	// and -1 if incorrect.
	behavioral_score bs(_size);
	boost::transform(hypoth, _target, bs.begin(),
	                 [](score_t hyp, bool b2) {
		                 bool b1 = (hyp > 0.0) ? true : false;
		                 return -score_t(b1 != b2);
	                 });
	return bs;
}

behavioral_score logical_bscore::best_possible_bscore() const
{
	return behavioral_score(_size, 0);
}

behavioral_score logical_bscore::worst_possible_bscore() const
{
	return behavioral_score(_size, -1);
}

score_t logical_bscore::min_improv() const
{
	return 0.5;
}

score_t logical_bscore::get_error(const behavioral_score &bs) const
{
	// Its minus the score: 0.0 is perfect score, 1.0 is worst score.
	return -sum_bscore(bs) / ((score_t) _size);
}

behavioral_score logical_bscore::operator()(const Handle &handle) const
{
	combo::complete_truth_table tt(handle, _arity);
	behavioral_score bs(_size);

	// Compare the predictions of the tree to that of the desired
	// result. A correct prdiction gets a score of 0, an incorrect
	// prediction gets a score of -1.
	boost::transform(tt, _target, bs.begin(), [](bool b1, bool b2) {
		return -score_t(b1 != b2);
	});

	return bs;
}

///////////////////
// contin_bscore //
///////////////////

behavioral_score contin_bscore::operator()(const combo_tree &tr) const
{
	// OTable target is the table of output we want to get.
	behavioral_score bs;

	// boost/range/algorithm/transform.
	// Take the input vectors cit, target, feed the elts to anon
	// funtion[] (which just computes square of the difference) and
	// put the results into bs.
	interpreter_visitor iv(tr);
	auto interpret_tr = boost::apply_visitor(iv);
	boost::transform(cti, target, back_inserter(bs),
	                 [&](const multi_type_seq &mts, const vertex &v) {
		                 contin_t tar = get_contin(v),
				                 res = get_contin(interpret_tr(mts.get_variant()));
		                 return -err_func(res, tar);
	                 });

	log_candidate_bscore(tr, bs);

	return bs;
}

behavioral_score contin_bscore::operator()(const Handle &handle) const
{
	behavioral_score bs;
	atomese::Interpreter interpreter(Constants::value_key, _size);

	const ValuePtr result = interpreter(handle);
	std::string test1 = oc_to_string(handle);
	vector<double> res = FloatValueCast(result)->value();
	if(handle->get_type()==NUMBER_NODE) {
		vector<double> tmp(_size,res.at(0));
		res=tmp;
	}
	boost::transform(res, target, back_inserter(bs),
	          [&](contin_t res, const vertex &tar_ver){
		          contin_t tar = get_contin(tar_ver);
		          return -err_func(res, tar);
	          });

	return bs;
}

behavioral_score contin_bscore::best_possible_bscore() const
{
	return behavioral_score(target.size(), 0);
}

score_t contin_bscore::min_improv() const
{
	// The backwards compat version of this is 0.0.  But for
	// continuously-variable scores, this is crazy, as the
	// system falls into a state of tweaking the tenth decimal place,
	// Limit any such tweaking to 4 decimal places of precision.
	// (thus 1e-4 below).
	//
	// Note: positive min_improv is taken as an absolute score.
	// Negative min_improve is treated as a relative score.
	return -1.0e-4;
}

void contin_bscore::set_complexity_coef(unsigned alphabet_size, float stdev)
{
	_complexity_coef = 0.0;
	if (stdev > 0.0)
		_complexity_coef = contin_complexity_coef(alphabet_size, stdev);

	logger().info() << "contin_bscore noise = " << stdev
	                << " alphabest size = " << alphabet_size
	                << " complexity ratio = " << 1.0 / _complexity_coef;
}

//////////////////////////////
// discretize_contin_bscore //
//////////////////////////////

discretize_contin_bscore::discretize_contin_bscore(const OTable &ot,
                                                   const ITable &it,
                                                   const vector<contin_t> &thres,
                                                   bool wa)
		: target(ot), cit(it), thresholds(thres), weighted_accuracy(wa),
		  classes(ot.size()), weights(thresholds.size() + 1, 1)
{
	// enforce that thresholds is sorted
	boost::sort(thresholds);
	// precompute classes
	boost::transform(target, classes.begin(), [&](const vertex &v) {
		return this->class_idx(get_contin(v));
	});
	// precompute weights
	multiset<size_t> cs(classes.begin(), classes.end());
	if (weighted_accuracy)
		for (size_t i = 0; i < weights.size(); ++i)
			weights[i] = classes.size() / (float) (weights.size() * cs.count(i));

	OC_ASSERT(ot.size() == it.size(),
	          "Error: discretize_contin_bscore: input and output table size do not match: %d %d",
	          it.size(), ot.size());
	_size = ot.size();
}

behavioral_score discretize_contin_bscore::best_possible_bscore() const
{
	return behavioral_score(target.size(), 0);
}

score_t discretize_contin_bscore::min_improv() const
{
	// not necessarily right, just the backwards-compat behavior
	return 0.0;
}

size_t discretize_contin_bscore::class_idx(contin_t v) const
{
	if (v < thresholds[0])
		return 0;
	size_t s = thresholds.size();
	if (v >= thresholds[s - 1])
		return s;
	return class_idx_within(v, 1, s);
}

size_t discretize_contin_bscore::class_idx_within(contin_t v,
                                                  size_t l_idx,
                                                  size_t u_idx) const
{
	// base case
	if (u_idx - l_idx == 1)
		return l_idx;
	// recursive case
	size_t m_idx = l_idx + (u_idx - l_idx) / 2;
	contin_t t = thresholds[m_idx - 1];
	if (v < t)
		return class_idx_within(v, l_idx, m_idx);
	else
		return class_idx_within(v, m_idx, u_idx);
}

behavioral_score discretize_contin_bscore::operator()(const combo_tree &tr) const
{
	/// @todo could be optimized by avoiding computing the OTable and
	/// directly using the results on the fly. On really big table
	/// (dozens of thousands of data points and about 100 inputs, this
	/// has overhead of about 10% of the overall time)
	OTable ct(tr, cit);
	behavioral_score bs(target.size());
	boost::transform(ct, classes, bs.begin(), [&](const vertex &v, size_t c_idx) {
		return -((c_idx != this->class_idx(get_contin(v))) * this->weights[c_idx]);
	});

	log_candidate_bscore(tr, bs);
	return bs;
}

behavioral_score discretize_contin_bscore::operator()(const Handle &handle) const
{
	behavioral_score bs;
	atomese::Interpreter interpreter(Constants::value_key, _size);

	const ValuePtr result = interpreter(handle);
	boost::transform(FloatValueCast(result)->value(), classes, back_inserter(bs),
	                 [&](contin_t res, size_t c_idx){
		                 return -((c_idx != this->class_idx(res)) * this->weights[c_idx]);
	                 });
	log_candidate_bscore(handle, bs);
	return bs;
}

///////////////////////////////////
// compressed_truth_table_bscore //
///////////////////////////////////

behavioral_score compressed_truth_table_bscore::operator()(const combo_tree &tr) const
{
	behavioral_score bs;

	interpreter_visitor iv(tr);
	auto interpret_tr = boost::apply_visitor(iv);
	// Evaluate the bscore components for all rows of the ctable
	for (const CompressedTable::value_type &vct : _wrk_ctable) {
		const CompressedTable::counter_t &c = vct.second;
		score_t sc = c.get(negate_vertex(interpret_tr(vct.first.get_variant())));
		bs.push_back(-sc);
	}

	if (_return_weighted_score) { // if boosting
		// Report the score only relative to the best-possible score.
		bs -= _best_possible_score;
	}

	log_candidate_bscore(tr, bs);

	return bs;
}

behavioral_score compressed_truth_table_bscore::operator()(const Handle &handle) const
{
	behavioral_score bs;
	atomese::Interpreter interpreter(Constants::compressed_value_key, _size);

	const ValuePtr result = interpreter(handle);
	boost::transform(LinkValueCast(result)->value(), _wrk_ctable, back_inserter(bs),
	                 [&](ValuePtr res, const CompressedTable::value_type &vct){
		                 const CompressedTable::counter_t &c = vct.second;
		                 // if predicted true return negative of arity of false
		                 // and vice versa
		                 return -c.get(bool_value_to_bool(res) ?
		                               id::logical_false : id::logical_true);
	                 });
	return bs;
}

/// Boolean ensemble scorer.  Assumes that the ensemble signature outputs
/// a boolean value.  All of the trees in the ensmble get a weighted vote,
/// that vote is totalled to get the prediction of the ensemble, and then
/// compared to the desired output.
behavioral_score compressed_truth_table_bscore::operator()(const scored_combo_tree_set &ensemble) const
{
	size_t sz = _wrk_ctable.size();

	// Step 1: accumulate the weighted prediction of each tree in
	// the ensemble.
	behavioral_score hypoth(sz, 0.0);
	for (const scored_combo_tree &sct: ensemble) {
		// apply each tree, in turn.
		interpreter_visitor iv(sct.get_tree());
		auto interpret_tr = boost::apply_visitor(iv);
		score_t weight = sct.get_weight();

		// Evaluate the tree for all rows of the ctable
		size_t i = 0;
		for (const CompressedTable::value_type &vct : _wrk_ctable) {
			// Add +1 if prediction is up and -1 if prediction is down.
			vertex prediction(interpret_tr(vct.first.get_variant()));
			hypoth[i] += (id::logical_true == prediction) ? weight : -weight;
			i++;
		}
	}

	// Step 2: compare the prediction of the ensemble to the desired
	// result. The array "hypoth" is positive to predict up, and
	// negative to predict down.  The compressed table holds the
	// (possibly weighted) count of up and down values; in the limit
	// of an uncompressed table, the number of 'wrong' answers is
	// the count of the iverted prediction.
	behavioral_score bs(sz);
	size_t i = 0;
	for (const CompressedTable::value_type &vct : _wrk_ctable) {
		const CompressedTable::counter_t &cnt = vct.second;
		vertex inverted_prediction = (hypoth[i] > 0.0) ?
		                             id::logical_false : id::logical_true;
		bs[i] = -cnt.get(inverted_prediction);
		i++;
	}

	if (_return_weighted_score) { // if boosting
		// Report the score only relative to the best-possible score.
		bs -= _best_possible_score;
	}

	return bs;
}

void compressed_truth_table_bscore::set_best_possible_bscore() const
{
	_best_possible_score.clear();
	transform(_wrk_ctable | map_values,
	          back_inserter(_best_possible_score),
	          [](const CompressedTable::counter_t &c) {
		          // OK, this looks like magic, but here's what it does:
		          // CompressedTable is a compressed table; different rows may
		          // have identical inputs, differing only in output.
		          // Clearly, in such a case, both outputs cannot be
		          // simultanously satisfied, but we can try to satisfy
		          // the one of which there is more (the max).  Thus,
		          // we take the fmin of the two possiblities as the
		          // number of wrong answers we are doomed to get.
		          //
		          // fmin is used, because the rows may be weighted
		          // by fractional values. This gives the correct
		          // result for weighted datasets: the most heavily
		          // weighted outcome is the prefered one.
		          return -score_t(fmin(c.get(id::logical_true),
		                               c.get(id::logical_false)));
	          });

	logger().info() << "compressed_truth_table_bscore: Best possible: "
	                << _best_possible_score;
}

behavioral_score compressed_truth_table_bscore::best_possible_bscore() const
{
	if (_return_weighted_score) {
		// The returned best score will always be zero, because the actual
		// best score is subtracted; this is required to get boosting to
		// work.
		return behavioral_score(_size, 0.0);
	}
	set_best_possible_bscore();
	return _best_possible_score;
}

behavioral_score compressed_truth_table_bscore::worst_possible_bscore() const
{
	behavioral_score bs;
	for (const CompressedTable::value_type &vct : _wrk_ctable) {
		const CompressedTable::counter_t &cnt = vct.second;

		// The most that the score can improve is to flip true to false,
		// or v.v. The worst score is to get the majority wrong.  This
		// workes correctly even for weighted tables, where the counts
		// could be arbitrary float-point values.
		score_t w = fabs(cnt.get(id::logical_true) -
		                 cnt.get(id::logical_false));
		bs.push_back(-w);
	}
	return bs;
}

score_t compressed_truth_table_bscore::min_improv() const
{
	// A return value of 0.5 would be correct only if all rows had
	// a weight of exactly 1.0.  Otherwise, we look for the row with
	// the smallest (positive) weight, and return that.
	// return 0.5;

	score_t min_weight = FLT_MAX;
	for (const CompressedTable::value_type &vct : _wrk_ctable) {
		const CompressedTable::counter_t &cnt = vct.second;

		// The most that the score can improve is to flip
		// true to false, or v.v.
		score_t w = fabs(cnt.get(id::logical_true) -
		                 cnt.get(id::logical_false));
		if (w != 0.0 and w < min_weight) min_weight = w;
	}
	return 0.5 * min_weight;
}


/////////////////////////
// enum_table_bscore //
/////////////////////////

behavioral_score enum_table_bscore::operator()(const combo_tree &tr) const
{
	behavioral_score bs;

	// Evaluate the bscore components for all rows of the ctable
	interpreter_visitor iv(tr);
	auto interpret_tr = boost::apply_visitor(iv);
	for (const CompressedTable::value_type &vct : _ctable) {
		const CompressedTable::counter_t &c = vct.second;
		// The number that are wrong equals total minus num correct.
		score_t sc = score_t(c.get(interpret_tr(vct.first.get_variant())));
		sc -= score_t(c.total_count());
		bs.push_back(sc);
	}

	log_candidate_bscore(tr, bs);
	return bs;
}

behavioral_score enum_table_bscore::operator()(const Handle &handle) const
{
	behavioral_score bs;

	atomese::Interpreter interpreter(Constants::compressed_value_key, _size);
	const ValuePtr result = interpreter(handle);
	boost::transform(LinkValueCast(result)->value(),
	                 _ctable, back_inserter(bs),
	                 [&](ValuePtr res, const CompressedTable::value_type &vct) {
		                 const CompressedTable::counter_t &c = vct.second;
		                 // The number that are wrong  equals
		                 // total minus num correct.
		                 score_t sc = score_t(c.get(value_to_enum(res)));
		                 sc -= score_t(c.total_count());
		                 return sc;
	                 }
	);

	return bs;
}

behavioral_score enum_table_bscore::best_possible_bscore() const
{
	behavioral_score bs;
	transform(_ctable | map_values, back_inserter(bs),
	          [](const CompressedTable::counter_t &c) {
		          // OK, this looks like magic, but here's what it does:
		          // CompressedTable is a compressed table; multiple rows may
		          // have identical inputs, differing only in output.
		          // Clearly, in such a case, different outputs cannot be
		          // simultanously satisfied, but we can try to satisfy
		          // the one of which there is the most.
		          unsigned most = 0;
		          CompressedTable::counter_t::const_iterator it = c.begin();
		          for (; it != c.end(); ++it) {
			          if (most < it->second) most = it->second;
		          }
		          return score_t(most - c.total_count());
	          });

	return bs;
}

score_t enum_table_bscore::min_improv() const
{
	return 0.5;
}

/////////////////////////
// enum_filter_bscore //
/////////////////////////

behavioral_score enum_filter_bscore::operator()(const combo_tree &tr) const
{
	behavioral_score bs;

	typedef combo_tree::sibling_iterator sib_it;
	typedef combo_tree::iterator pre_it;

	pre_it it = tr.begin();
	if (is_enum_type(*it))
		return enum_table_bscore::operator()(tr);

	OC_ASSERT(*it == id::cond, "Error: unexpcected candidate!");
	sib_it predicate = it.begin();
	vertex consequent = *next(predicate);

	// Evaluate the bscore components for all rows of the ctable
	interpreter_visitor iv_tr(tr), iv_predicate(predicate);
	auto interpret_tr = boost::apply_visitor(iv_tr);
	auto interpret_predicate = boost::apply_visitor(iv_predicate);
	for (const CompressedTable::value_type &vct : _ctable) {
		const CompressedTable::counter_t &c = vct.second;

		unsigned total = c.total_count();

		// The number that are wrong equals total minus num correct.
		score_t sc = score_t(c.get(interpret_tr(vct.first.get_variant())));
		sc -= score_t(total);

		// Punish the first predicate, if it is wrong.
		vertex pr = interpret_predicate(vct.first.get_variant());
		if (pr == id::logical_true) {
			if (total != c.get(consequent))
				sc -= punish * total;
		}

		bs.push_back(sc);
	}

	log_candidate_bscore(tr, bs);
	return bs;
}

/////////////////////////
// enum_graded_bscore //
/////////////////////////

/// OK, the goal here is to compute the "graded" tree complexity.
/// Much the same way as the score is graded below, we want to do
/// the same for the complexity, so that complex later predicates
/// don't (do?) dominate the the penalty.  Actually, this is
/// retro-graded: punish more complex, later predicates...
score_t enum_graded_bscore::graded_complexity(combo_tree::iterator it) const
{
	typedef combo_tree::sibling_iterator sib_it;
	typedef combo_tree::iterator pre_it;

	if (it.is_childless()) return 0.0;
	sib_it predicate = it.begin();
	score_t cpxy = 0.0;
	score_t weight = 1.0;
	while (1) {
		cpxy += weight * score_t(tree_complexity((pre_it) predicate));

		// Is it the last one, the else clause?
		if (is_enum_type(*predicate))
			break;

		// advance
		predicate = next(predicate, 2);
		weight /= grading;

	}
	return cpxy;
}

behavioral_score enum_graded_bscore::operator()(const combo_tree &tr) const
{
	behavioral_score bs;

	typedef combo_tree::sibling_iterator sib_it;
	typedef combo_tree::iterator pre_it;

	pre_it it = tr.begin();
	if (is_enum_type(*it))
		return enum_table_bscore::operator()(tr);

	OC_ASSERT(*it == id::cond, "Error: unexpected candidate!");

	// Evaluate the bscore components for all rows of the ctable
	// TODO
	sib_it predicate = it.begin();
	for (const CompressedTable::value_type &vct : _ctable) {
		const CompressedTable::counter_t &c = vct.second;

		unsigned total = c.total_count();
		score_t weight = 1.0;

		// The number that are wrong equals total minus num correct.
		score_t sc = -score_t(total);
		while (1) {
			// Is it the last one, the else clause?
			if (is_enum_type(*predicate)) {
				vertex consequent = *predicate;
				sc += c.get(consequent);
				sc *= weight;
				break;
			}

			// The first true predicate terminates.
			interpreter_visitor iv(predicate);
			vertex pr = boost::apply_visitor(iv, vct.first.get_variant());
			if (pr == id::logical_true) {
				vertex consequent = *next(predicate);
				sc += c.get(consequent);
				sc *= weight;
				break;
			}

			// advance
			predicate = next(predicate, 2);
			weight *= grading;
		}
		bs.push_back(sc);
	}

	log_candidate_bscore(tr, bs);
	return bs;
}

complexity_t enum_graded_bscore::get_complexity(const combo::combo_tree &tr) const
{
	return graded_complexity(tr.begin());
}

score_t enum_graded_bscore::min_improv() const
{
	// Negative values are interpreted as percentages by the optimizer.
	// So -0.05 means "a 5% improvement".  Problem is, the grading
	// wrecks any sense of an absolute score improvement...
	return -0.05;
}

// Much like enum_graded_score, above, except that we exchange the
// inner and outer loops.  This makes the algo slower and bulkier, but
// it does allow the effectiveness of predicates to be tracked.
//
behavioral_score enum_effective_bscore::operator()(const combo_tree &tr) const
{

	typedef combo_tree::sibling_iterator sib_it;
	typedef combo_tree::iterator pre_it;

	behavioral_score bs(_ctable_usize);

	// Is this just a constant? Then just add them up.
	pre_it it = tr.begin();
	if (is_enum_type(*it)) {
		behavioral_score::iterator bit = bs.begin();
		for (const CompressedTable::value_type &vct : _ctable) {
			const CompressedTable::counter_t &c = vct.second;

			// The number that are wrong equals total minus num correct.
			*bit++ = c.get(*it) - score_t(c.total_count());
		}
		return bs;
	}

	OC_ASSERT(*it == id::cond, "Error: unexpcected candidate!");

	// Accumulate the score with multiple passes, so zero them out here.
	for (score_t &sc : bs) sc = 0.0;

	// Are we done yet?
	vector<bool> done(_ctable_usize);
	vector<bool>::iterator dit = done.begin();
	for (; dit != done.end(); ++dit) *dit = false;

	sib_it predicate = it.begin();
	score_t weight = 1.0;
	while (1) {

		// Is it the last one, the else clause?
		if (is_enum_type(*predicate)) {
			vertex consequent = *predicate;

			behavioral_score::iterator bit = bs.begin();
			vector<bool>::iterator dit = done.begin();
			for (const CompressedTable::value_type &vct : _ctable) {
				if (*dit == false) {
					const CompressedTable::counter_t &c = vct.second;

					// The number that are wrong equals total minus num correct.
					score_t sc = -score_t(c.total_count());
					sc += c.get(consequent);
					*bit += weight * sc;
				}
				++bit;
				++dit;
			}
			break;
		}

		vertex consequent = *next(predicate);

		// Evaluate the bscore components for all rows of the ctable
		behavioral_score::iterator bit = bs.begin();
		vector<bool>::iterator dit = done.begin();

		bool effective = false;
		interpreter_visitor iv(predicate);
		auto interpret_predicate = boost::apply_visitor(iv);
		for (const CompressedTable::value_type &vct : _ctable) {
			if (*dit == false) {
				vertex pr = interpret_predicate(vct.first.get_variant());
				if (pr == id::logical_true) {
					const CompressedTable::counter_t &c = vct.second;
					int sc = c.get(consequent);
					// A predicate is effective if it evaluates to true,
					// and at least gets a right answr when it does...
					if (0 != sc) effective = true;

					// The number that are wrong equals total minus num correct.
					sc -= c.total_count();
					*bit += weight * score_t(sc);

					*dit = true;
				}
			}
			++bit;
			++dit;
		}

		// advance
		predicate = next(predicate, 2);
		if (effective) weight *= grading;
	}

	log_candidate_bscore(tr, bs);
	return bs;
}

//////////////////////////////////
// interesting_predicate_bscore //
//////////////////////////////////

interesting_predicate_bscore::interesting_predicate_bscore(const CompressedTable &ctable_,
                                                           weight_t kld_w_,
                                                           weight_t skewness_w_,
                                                           weight_t stdU_w_,
                                                           weight_t skew_U_w_,
                                                           score_t min_activation_,
                                                           score_t max_activation_,
                                                           score_t penalty_,
                                                           bool positive_,
                                                           bool abs_skewness_,
                                                           bool decompose_kld_)
		: _ctable(ctable_),
		  _kld_w(kld_w_), _skewness_w(skewness_w_), _abs_skewness(abs_skewness_),
		  _stdU_w(stdU_w_), _skew_U_w(skew_U_w_), _min_activation(min_activation_),
		  _max_activation(max_activation_), _penalty(penalty_), _positive(positive_),
		  _decompose_kld(decompose_kld_)
{
	// Define counter (mapping between observation and its number of occurences)
	// That is, create a historgram showing how often each output value
	// occurs in the ctable.
	boost::for_each(_ctable | map_values, [this](const CompressedTable::mapped_type &mv) {
		boost::for_each(mv, [this](const CompressedTable::counter_t::value_type &v) {
			_counter[get_contin(v.first.value)] += v.second;
		});
	});

	// Precompute pdf (probability distribution function)
	if (_kld_w > 0) {
		_pdf = _counter;
		_klds.set_p_pdf(_pdf);

		// Compute the skewness of the pdf
		accumulator_t acc;
		for (const auto &v : _pdf)
			acc(v.first, weight = v.second);
		_skewness = weighted_skewness(acc);
		logger().debug("interesting_predicate_bscore::_skewness = %f", _skewness);
	}
}

behavioral_score interesting_predicate_bscore::operator()(const combo_tree &tr) const
{
	// OK, here's the deal. The combo tree evaluates to T/F on each
	// input table row. That is, the combo tree is a predicate that
	// selects certain rows of the input table.  Here, pred_cache is just
	// a cache, to avoid multiple evaluations of the combo tree: its
	// just a table with just one column, equal to the value of the
	// combo tree on each input row.
	OTable pred_cache(tr, _ctable);

	// target simply negates (inverts) the predicate.
	vertex target = bool_to_vertex(_positive);

	// Count how many rows the predicate selected.
	unsigned total = 0;   // total number of observations (could be optimized)
	unsigned actives = 0; // total number of positive (or negative if
	// positive is false) predicate values
	boost::for_each(_ctable | map_values, pred_cache,
	                [&](const CompressedTable::counter_t &c, const vertex &v) {
		                unsigned tc = c.total_count();
		                if (v == target)
			                actives += tc;
		                total += tc;
	                });

	logger().fine("ip scorer: total = %u", total);
	logger().fine("ip scorer: actives = %u", actives);

	// Create a histogram of output values, ignoring non-selected rows.
	// Do this by filtering the ctable output column according to the,
	// predicate, discarding non-selected rows. Then total up how often
	// each distinct output value occurs.
	counter_t pred_counter;
	boost::for_each(_ctable | map_values, pred_cache,
	                [&](const CompressedTable::counter_t &c, const vertex &v) {
		                if (v == target) {
			                for (const auto &mv : c)
				                pred_counter[get_contin(mv.first.value)] = mv.second;
		                }
	                });

	logger().fine("ip scorer: pred_cache.size() = %u", pred_cache.size());
	logger().fine("ip scorer: pred_counter.size() = %u", pred_counter.size());

	// If there's only one output value left, then punt.  Statistics
	// like skewness need a distribution that isn't a single spike.
	if (pred_counter.size() == 1) {
		behavioral_score bs;
		bs.push_back(very_worst_score);
		log_candidate_bscore(tr, bs);
		return bs;
	}

	behavioral_score bs;
	// Compute Kullback-Leibler divergence (KLD) of the filetered
	// distribution.
	if (_kld_w > 0.0) {
		if (_decompose_kld) {
			_klds(pred_counter, back_inserter(bs));
			boost::transform(bs, bs.begin(), _kld_w * arg1);
		} else {
			score_t pred_klds = _klds(pred_counter);
			logger().fine("ip scorer: klds = %f", pred_klds);
			bs.push_back(_kld_w * pred_klds);
		}
	}

	// Compute skewness of the filtered distribution.
	if (_skewness_w > 0 || _stdU_w > 0 || _skew_U_w > 0) {

		// Gather statistics with a boost accumulator
		accumulator_t acc;
		for (const auto &v : pred_counter)
			acc(v.first, weight = v.second);

		score_t diff_skewness = 0;
		if (_skewness_w > 0 || _skew_U_w > 0) {
			// push the absolute difference between the
			// unconditioned skewness and conditioned one
			score_t pred_skewness = weighted_skewness(acc);
			diff_skewness = pred_skewness - _skewness;
			score_t val_skewness = (_abs_skewness ?
			                        abs(diff_skewness) :
			                        diff_skewness);
			logger().fine("ip scorer: pred_skewness = %f", pred_skewness);
			if (_skewness_w > 0)
				bs.push_back(_skewness_w * val_skewness);
		}

		score_t stdU = 0;
		if (_stdU_w > 0 || _skew_U_w > 0) {

			// Compute the standardized Mann–Whitney U
			stdU = standardizedMannWhitneyU(_counter, pred_counter);
			logger().fine("ip scorer: stdU = %f", stdU);
			if (_stdU_w > 0.0)
				bs.push_back(_stdU_w * abs(stdU));
		}

		// push the product of the relative differences of the
		// shift (stdU) and the skewness (so that if both go
		// in the same direction the value if positive, and
		// negative otherwise)
		if (_skew_U_w > 0)
			bs.push_back(_skew_U_w * stdU * diff_skewness);
	}

	// add activation_penalty component
	score_t activation = actives / (score_t) total;
	score_t activation_penalty = get_activation_penalty(activation);
	logger().fine("ip scorer: activation = %f", activation);
	logger().fine("ip scorer: activation penalty = %e", activation_penalty);
	bs.push_back(activation_penalty);

	log_candidate_bscore(tr, bs);
	return bs;
}

behavioral_score interesting_predicate_bscore::best_possible_bscore() const
{
	return behavioral_score(1, very_best_score);
}

void interesting_predicate_bscore::set_complexity_coef(unsigned alphabet_size,
                                                       float stdev)
{
	_complexity_coef = 0.0;
	if (stdev > 0)
		_complexity_coef = contin_complexity_coef(alphabet_size, stdev);

	logger().info() << "intersting_predicate_bscore noise = " << stdev
	                << " alphabest size = " << alphabet_size
	                << " complexity ratio = " << 1.0 / _complexity_coef;
}

score_t interesting_predicate_bscore::get_activation_penalty(score_t activation) const
{
	score_t dst = fmax(fmax(_min_activation - activation, score_t(0))
	                   / _min_activation,
	                   fmax(activation - _max_activation, score_t(0))
	                   / (1.0 - _max_activation));
	logger().fine("ip scorer: dst = %f", dst);
	return log(pow((1.0 - dst), _penalty));
}

score_t interesting_predicate_bscore::min_improv() const
{
	return 0.0;                 // not necessarily right, just the
	// backwards-compatible behavior
}

// ====================================================================

/// Cluster scoring.
/// Experimental attempt at using moses for cluster discovery.
/// When this scorer is presented with a combo tree, it attempts
/// to see if the tree, when applied to the input table, naturally
/// clusters scores into disparate groups.  Since the combo tree,
/// applied to the input table, results in a 1-d array of values,
/// the clustering is judged by using the one-dimensional k-means
/// clustering algo.
///
/// This is considered experimental because it doesn't yet work
/// very well, is likely to be redisigned, and finally, doesn't
/// even output all the data that is required to use the resulting
/// formula (the edges, with are printed by hand, below).
///
/// XXX this should probably be removed! TODO FIXME

cluster_bscore::cluster_bscore(const ITable &itable)
		: _itable(itable)
{
}

behavioral_score cluster_bscore::operator()(const combo_tree &tr) const
{
	// evaluate the tree on the table
	OTable oned(tr, _itable);

	size_t nclusters = 3;

	OC_ASSERT(nclusters < oned.size());

	// Initial guess for the centroids
	vector<score_t> centers(nclusters);
	size_t i;
	for (i = 0; i < nclusters; i++) {
		centers[i] = get_contin(oned[i]);
	}
	std::sort(centers.begin(), centers.end());

	vector<score_t> edges(nclusters);
	for (i = 0; i < nclusters - 1; i++) {
		edges[i] = 0.5 * (centers[i] + centers[i + 1]);
	}
	edges[nclusters - 1] = INFINITY;

	// sort the values. This makes assignment easier.
	size_t numvals = oned.size();
	vector<score_t> vals(numvals);
	size_t j;
	for (j = 0; j < numvals; j++)
		vals[j] = get_contin(oned[j]);
	std::sort(vals.begin(), vals.end());

	// One-dimensional k-means algorithm (LLoyd's algorithm)
	vector<size_t> edge_idx(nclusters - 1);
	bool changed = true;
	while (changed) {
		vector<score_t> cnt(nclusters);
		vector<score_t> sum(nclusters);
		changed = false;
		i = 0;
		for (j = 0; j < numvals; j++) {
			score_t sc = vals[j];

			if (std::isinf(sc) || std::isnan(sc)) {
				behavioral_score bs;
				bs.push_back(-INFINITY);
				return bs;
			}

			if (sc <= edges[i]) {
				cnt[i] += 1.0;
				sum[i] += sc;
			} else {
				OC_ASSERT(i < nclusters - 1);
				if (j != edge_idx[i]) changed = true;
				edge_idx[i] = j;
				i++;
			}
		}

		// Compute cluster centers.
		for (i = 0; i < nclusters; i++) {
			// A cluster must have at least two points in it,
			// as otherwise the RMS would be zero.  Heck, lets make it three.
			if (cnt[i] < 3.5) {
				behavioral_score bs;
				bs.push_back(-INFINITY);
				return bs;
			}
			sum[i] /= cnt[i];
		}
		for (i = 0; i < nclusters - 1; i++) edges[i] = 0.5 * (sum[i] + sum[i + 1]);
	}


	// Compute the RMS width of each cluster.
	score_t final = 0.0;
	score_t cnt = 0.0;
	score_t sum = 0.0;
	score_t squ = 0.0;
	i = 0;
	for (j = 0; j < numvals; j++) {
		score_t sc = vals[j];
		if (sc <= edges[i]) {
			cnt += 1.0;
			sum += sc;
			squ += sc * sc;
		} else {
			sum /= cnt;
			squ /= cnt;

			final += squ - sum * sum;
			i++;
			cnt = 0.0;
			sum = 0.0;
			squ = 0.0;
		}
	}

	// normalize by bind-width
	final = sqrt(final);
	// score_t binwidth = edges[nclusters-2] - edges[0];
	score_t binwidth = vals[numvals - 1] - vals[0];
	final /= binwidth;
	// The narrower the peaks, the higher the score.
	// This way of doing it works better with the complexity penalty
	final = 1.0 / final;

	behavioral_score bs;
	bs.push_back(final);
#if 0
	if (final > 80) {
		logger().debug() << "cluster tr="<<tr<< "\ncluster final score=" << final << std::endl;
		for (i=0; i<nclusters-1; i++)
			logger().debug("cluster edges:  %d %f", i, edges[i]);
		for (j=0; j<numvals; j++)
			logger().debug("cluster point: %f", vals[j]);
	}
#endif
	log_candidate_bscore(tr, bs);
	return bs;
}

// Return the best possible bscore. Used as one of the
// termination conditions (when the best bscore is reached).
behavioral_score cluster_bscore::best_possible_bscore() const
{
	return behavioral_score(1, very_best_score);
}

score_t cluster_bscore::min_improv() const
{
	return 0.1;
}

} // ~namespace moses
} // ~namespace opencog
