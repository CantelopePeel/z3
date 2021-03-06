/*++
  Copyright (c) 2020 Microsoft Corporation

  Module Name:

   sat_lut_finder.cpp

  Abstract:
   
    lut finder

  Author:

    Nikolaj Bjorner 2020-01-02

  Notes:
  

  --*/

#include "sat/sat_lut_finder.h"
#include "sat/sat_solver.h"

namespace sat {

    void lut_finder::operator()(clause_vector& clauses) {
        m_removed_clauses.reset();
        unsigned max_size = m_max_lut_size;
        // we better have enough bits in the combination mask to 
        // handle clauses up to max_size.
        // max_size = 5 -> 32 bits
        // max_size = 6 -> 64 bits
        SASSERT(sizeof(m_combination)*8 >= (1ull << static_cast<uint64_t>(max_size)));
        init_clause_filter();
        for (unsigned i = 0; i <= 6; ++i) init_mask(i);
        m_var_position.resize(s.num_vars());
        for (clause* cp : clauses) {
            cp->unmark_used();
        }
        for (; max_size > 2; --max_size) {
            for (clause* cp : clauses) {
                clause& c = *cp;
                if (c.size() == max_size && !c.was_removed() && !c.is_learned() && !c.was_used()) {
                    check_lut(c);
                }
            }
        }
        m_clause_filters.clear();
        
        for (clause* cp : clauses) cp->unmark_used();
        for (clause* cp : m_removed_clauses) cp->mark_used();
        std::function<bool(clause*)> not_used = [](clause* cp) { return !cp->was_used(); };
        clauses.filter_update(not_used);
    }

    void lut_finder::check_lut(clause& c) {
        SASSERT(c.size() > 2);
        unsigned filter = get_clause_filter(c);
        s.init_visited();
        unsigned mask = 0, i = 0;        
        m_vars.reset();
        for (literal l : c) {
            m_vars.push_back(l.var());
            m_var_position[l.var()] = i;
            s.mark_visited(l.var());
            mask |= (l.sign() << (i++)); 
        }
        m_clauses_to_remove.reset();
        m_clauses_to_remove.push_back(&c);
        m_clause.resize(c.size());
        m_combination = 0;
        m_num_combinations = 0;
        set_combination(mask);
        c.mark_used();
        for (literal l : c) {
            for (auto const& cf : m_clause_filters[l.var()]) {                
                if ((filter == (filter | cf.m_filter)) && 
                    !cf.m_clause->was_used() &&
                    extract_lut(*cf.m_clause)) {
                    add_lut();
                    return;
                }
            }
            // TBD: replace by BIG
            // loop over binary clauses in watch list
            for (watched const & w : s.get_wlist(l)) {
                if (w.is_binary_clause() && s.is_visited(w.get_literal().var()) && w.get_literal().index() < l.index()) {
                    if (extract_lut(~l, w.get_literal())) {
                        add_lut();
                        return;
                    }
                }
            }
            l.neg();
            for (watched const & w : s.get_wlist(l)) {
                if (w.is_binary_clause() && s.is_visited(w.get_literal().var()) && w.get_literal().index() < l.index()) {
                    if (extract_lut(~l, w.get_literal())) {
                        add_lut();
                        return;
                    }
                }
            }
        }
    }

    void lut_finder::add_lut() {
        DEBUG_CODE(for (clause* cp : m_clauses_to_remove) VERIFY(cp->was_used()););
        m_removed_clauses.append(m_clauses_to_remove);
        bool_var v;
        uint64_t lut = convert_combination(m_vars, v);
        m_on_lut(lut, m_vars, v);
    }

    bool lut_finder::extract_lut(literal l1, literal l2) {
        SASSERT(s.is_visited(l1.var()));
        SASSERT(s.is_visited(l2.var()));
        m_missing.reset();
        unsigned mask = 0;
        for (unsigned i = 0; i < m_vars.size(); ++i) {
            if (m_vars[i] == l1.var()) {
                mask |= (l1.sign() << i);
            }
            else if (m_vars[i] == l2.var()) {
                mask |= (l2.sign() << i);
            }
            else {
                m_missing.push_back(i);
            }
        }
        return update_combinations(mask);
    }

    bool lut_finder::extract_lut(clause& c2) {
        for (literal l : c2) {            
            if (!s.is_visited(l.var())) return false;
        }
        if (c2.size() == m_vars.size()) {
            m_clauses_to_remove.push_back(&c2);
            c2.mark_used();
        }
        // insert missing
        unsigned mask = 0;
        m_missing.reset();
        SASSERT(c2.size() <= m_vars.size());
        for (unsigned i = 0; i < m_vars.size(); ++i) {
            m_clause[i] = null_literal;
        }
        for (literal l : c2) {
            unsigned pos = m_var_position[l.var()];
            m_clause[pos] = l;
        }
        for (unsigned j = 0; j < m_vars.size(); ++j) {
            literal lit = m_clause[j];
            if (lit == null_literal) {
                m_missing.push_back(j);
            }
            else {
                mask |= (m_clause[j].sign() << j);
            }
        }                    
        return update_combinations(mask);
    }

    bool lut_finder::update_combinations(unsigned mask) {
        unsigned num_missing = m_missing.size();
        for (unsigned k = 0; k < (1ul << num_missing); ++k) {
            unsigned mask2 = mask;
            for (unsigned i = 0; i < num_missing; ++i) {
                if ((k & (1 << i)) != 0) {
                    mask2 |= 1ul << m_missing[i];
                }
            }
            set_combination(mask2);
        }
        return lut_is_defined(m_vars.size());
    }

    bool lut_finder::lut_is_defined(unsigned sz) {
        if (m_num_combinations < (1ull << (sz/2))) 
            return false;
        for (unsigned i = 0; i < sz; ++i) {
            if (lut_is_defined(i, sz)) 
                return true;
        }
        return false;
    }

    /**
     * \brief create the masks
     * i = 0: 101010101010101
     * i = 1: 1100110011001100
     * i = 2: 1111000011110000
     * i = 3: 111111110000000011111111
     */

    void lut_finder::init_mask(unsigned i) {
        SASSERT(i <= 6);
        uint64_t m = 0;
        if (i == 6) {
            m = ~((uint64_t)0);
        }
        else {
            m = (1ull << (1u << i)) - 1;   // i = 0: m = 1
            unsigned w = 1u << (i + 1);    // i = 0: w = 2
            while (w < 64) {
                m |= (m << w);             // i = 0: m = 1 + 4
                w *= 2;
            }
        }
        m_masks[i] = m;
    }

    /**
     * \brief check if all output combinations for variable i are defined.
     */
    bool lut_finder::lut_is_defined(unsigned i, unsigned sz) {
        uint64_t c = m_combination | (m_combination >> (1ull << (uint64_t)i));
        uint64_t m = m_masks[i];
        if (sz < 6) m &= ((1ull << sz) - 1);
        return (c & m) == m;
    }

    /**
     * find variable where it is defined
     * convert bit-mask to truth table for that variable.
     * remove variable from vars, 
     * return truth table.
     */

    uint64_t lut_finder::convert_combination(bool_var_vector& vars, bool_var& v) {
        SASSERT(lut_is_defined(vars.size()));
        unsigned i = 0, j = 0;
        for (; i < vars.size(); ++i) {
            if (lut_is_defined(i, vars.size())) {
                break;
            }
        }
        SASSERT(i < vars.size());
        v = vars[i];
        vars.erase(v);
        uint64_t r = 0;
        unsigned stride_sz = (1u << i);
        unsigned num_strides = (1u << vars.size()) / (stride_sz * 2);
        
        switch (i) {
        case 0:
            for (unsigned j = 0; j < (1u << vars.size()); ++j) {
                if (0 == (m_combination & (1ull << 2*j))) {
                    r |= (1ull << j);
                }
            }
            break;
        case 1:
            // (0, 2) (1, 3), (4, 6), (5, 7)
            for (unsigned j = 0; j < (1u << vars.size()); ++j) {
                
            }
            // TBD
            break;
        default:
            // TBD
            break;
        }
        return r;
    }

    void lut_finder::init_clause_filter() {
        m_clause_filters.reset();
        m_clause_filters.resize(s.num_vars());
        init_clause_filter(s.m_clauses);
        init_clause_filter(s.m_learned);
    }

    void lut_finder::init_clause_filter(clause_vector& clauses) {
        for (clause* cp : clauses) {
            clause& c = *cp;
            if (c.size() <= m_max_lut_size && s.all_distinct(c)) {
                clause_filter cf(get_clause_filter(c), cp);
                for (literal l : c) {
                    m_clause_filters[l.var()].push_back(cf);
                }            
            }
        }
    }

    unsigned lut_finder::get_clause_filter(clause const& c) {
        unsigned filter = 0;
        for (literal l : c) {
            filter |= 1 << ((l.var() % 32));
        }
        return filter;
    }


}
