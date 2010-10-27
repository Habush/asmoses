/** feature_scorer.h --- 
 *
 * Copyright (C) 2010 Nil Geisweiller
 *
 * Author: Nil Geisweiller <nilg@laptop>
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


#ifndef _OPENCOG_FEATURE_SCORER_H
#define _OPENCOG_FEATURE_SCORER_H

namespace opencog {

/**
 * Computes the conditional entropy
 *
 * H(Y|X1, ..., Xn) = H(Y, X1, ..., Xn) - H(X1, ..., Xn)
 *
 * of the output Y given a set of features X1, ..., Xn. IT (a matrix) is
 * the type of the input table, OT (a vector) is the type of the
 * output table. We actually the inverse, that is
 *
 * -H(Y|X1, ..., Xn)
 *
 * because the feature_selection assumes a score to maximize
 */
template<IT, OT>
struct ConditionalEntropy {
    ConditionalEntropy(const IT& it, const OT& ot) 
        : _it(it), _ot(ot) {}

    template<FeatureSet> double operator()(const FeatureSet& features) {
        typedef IT::value_type::value_type EntryT;
        // the following mapping is used to keep track of the number
        // of inputs a given setting. For instance X1=false, X2=true,
        // X3=true is one possible setting. It is then used to compute
        // H(Y, X1, ..., Xn) and H(X1, ..., Xn)
        typedef std::unordered_map<std::vector<EntryT>, unsigned int> TupleCount;
        TupleCount ic, // for H(X1, ..., Xn)
            ioc; // for H(Y, X1, ..., Xn)
        double total = ot.size();
        IT::const_iterator i_it = it.begin();
        OT::const_iterator o_it = ot.begin();
        for(; i_it != it.end(); i_it++, o_it++) {
            std::vector<EntryT> ic_vec;
            foreach(FeatureSet::value_type& idx, features)
                ic_vec.push_back(iv[idx]);
            ic[ic_vec]++;
            std::vector<EntryT> ioc_vec(ic);
            ioc_vec.push_back(*o_it);
            ioc[ioc_vec]++;
        }
        // Compute conditional entropy
        std::vector<double> ip, iop;
        foreach(const TupleCount::value_type& vic, ic)
            ip.push_back(vic.second/total);
        foreach(const TupleCount::value_type& vioc, ioc)
            iop.push_back(vioc.second/total);
        // we return -H(Y|X1, ..., Xn)
        return entropy(ip.begin(), ip.end()) - entropy(iop.begin(), iop.end());
    }

protected:
    const InputTable& it;
    const OutputTable& ot;
};

} // ~namespace opencog

#endif // _OPENCOG_FEATURE_SCORER_H
