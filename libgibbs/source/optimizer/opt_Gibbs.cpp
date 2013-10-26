/*=============================================================================
	Copyright (c) 2012-2013 Richard Otis

    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/

// opt_Gibbs.cpp -- definition for Gibbs energy optimizer

#include "libgibbs/include/libgibbs_pch.hpp"
#include "libgibbs/include/models.hpp"
#include "libgibbs/include/optimizer/optimizer.hpp"
#include "libgibbs/include/optimizer/opt_Gibbs.hpp"
#include "libgibbs/include/optimizer/halton.hpp"
#include "libtdb/include/logging.hpp"
#include "external/coin/IpTNLP.hpp"

using namespace Ipopt;

/* Constructor. */
GibbsOpt::GibbsOpt(
		const Database &DB,
		const evalconditions &sysstate) :
			conditions(sysstate),
			phase_iter(DB.get_phase_iterator()),
			phase_end(DB.get_phase_iterator_end())
{
	int varcount = 0;
	int activephases = 0;
	logger opt_log(journal::keywords::channel = "optimizer");

	for (auto i = DB.get_phase_iterator(); i != DB.get_phase_iterator_end(); ++i) {
		if (conditions.phases.find(i->first) != conditions.phases.end()) {
			if (conditions.phases.at(i->first) == PhaseStatus::ENTERED) phase_col[i->first] = i->second;
		}
	}



	phase_iter = phase_col.cbegin();
	phase_end = phase_col.cend();
	if (conditions.elements.cbegin() == conditions.elements.cend()) BOOST_LOG_SEV(opt_log, critical) << "Missing element conditions!";
	if (phase_iter == phase_end) BOOST_LOG_SEV(opt_log, critical) << "No phases found!";

	// build_variable_map() will fill main_indices
	main_ss = build_variable_map(phase_iter, phase_end, conditions, main_indices);

	// load the parameters from the database
	parameter_set pset = DB.get_parameter_set();

	// this is the part where we look up the models enabled for each phase and call their AST builders
	// then we build a master Gibbs AST for the objective function
	// TODO: right now the models called are hard-coded, in the future this will be typedef-dependent
	for (auto i = phase_iter; i != phase_end; ++i) {
		if (conditions.phases[i->first] != PhaseStatus::ENTERED) continue;
		++activephases;
		boost::spirit::utree phase_ast;
		boost::spirit::utree temptree;
		// build an AST for the given phase
		boost::spirit::utree curphaseref = PureCompoundEnergyModel(i->first, main_ss, pset).get_ast();
		BOOST_LOG_SEV(opt_log, debug) << i->first << "ref" << std::endl << curphaseref << std::endl;
		boost::spirit::utree idealmix = IdealMixingModel(i->first, main_ss).get_ast();
		BOOST_LOG_SEV(opt_log, debug) << i->first << "idmix" << std::endl << idealmix << std::endl;
		boost::spirit::utree redlichkister = RedlichKisterExcessEnergyModel(i->first, main_ss, pset).get_ast();
		BOOST_LOG_SEV(opt_log, debug) << i->first << "excess" << std::endl << redlichkister << std::endl;

		// sum the contributions
		phase_ast.push_back("+");
		phase_ast.push_back(idealmix);
		temptree.push_back("+");
		temptree.push_back(curphaseref);
		temptree.push_back(redlichkister);
		phase_ast.push_back(temptree);
		temptree.clear();

		// multiply by phase fraction variable
		temptree.push_back("*");
		temptree.push_back(i->first + "_FRAC");
		temptree.push_back(phase_ast);
		phase_ast.swap(temptree);
		temptree.clear();

		// add phase AST to master AST
		if (activephases != 1) {
			// this is not the only / first phase
			temptree.push_back("+");
			temptree.push_back(master_tree);
			temptree.push_back(phase_ast);
			master_tree.swap(temptree);
		}
		else master_tree.swap(phase_ast);

	}
	BOOST_LOG_SEV(opt_log, debug) << "master_tree: " << master_tree << std::endl;

	// Add the mandatory constraints to the ConstraintManager
	// NOTE: Once addConstraint has int return type, use it to record id's for g[] in optimizer
	cm.addConstraint(PhaseFractionBalanceConstraint(phase_iter, phase_end));
	// Add the mass balance constraint to ConstraintManager (mandatory)

	// TODO: Add any user-specified constraints to the ConstraintManager


	// Build a sitefracs object so that we can calculate the Gibbs energy
	for (auto i = phase_iter; i != phase_end; ++i) {
		if (conditions.phases[i->first] != PhaseStatus::ENTERED) continue;
		sublattice_vector subls_vec;
		for (auto j = i->second.get_sublattice_iterator(); j != i->second.get_sublattice_iterator_end();++j) {
			std::map<std::string,double> subl_map;
			for (auto k = (*j).get_species_iterator(); k != (*j).get_species_iterator_end();++k) {
				// Check if this species in this sublattice is on our list of elements to investigate
				if (std::find(conditions.elements.cbegin(),conditions.elements.cend(),*k) != conditions.elements.cend()) {
					subl_map[*k] = 1;
					//var_map.sitefrac_iters[std::distance(phase_iter,i)][std::distance(i->second.get_sublattice_iterator(),j)][*k]
				}
			}
			subls_vec.push_back(subl_map);
		}
		mysitefracs.push_back(std::make_pair(i->first,subls_vec));
	}
	// Build the index map
	for (auto i = phase_iter; i != phase_end; ++i) {
		if (conditions.phases[i->first] != PhaseStatus::ENTERED) continue;
		//std::cout << "x[" << varcount << "] = " << i->first << " phasefrac" << std::endl;
		var_map.phasefrac_iters.push_back(boost::make_tuple(varcount,varcount+1,i));
		++varcount;
		for (auto j = i->second.get_sublattice_iterator(); j != i->second.get_sublattice_iterator_end();++j) {
			for (auto k = (*j).get_species_iterator(); k != (*j).get_species_iterator_end();++k) {
				// Check if this species in this sublattice is on our list of elements to investigate
				if (std::find(conditions.elements.cbegin(),conditions.elements.cend(),*k) != conditions.elements.cend()) {
					// This site matches one of our elements under investigation
					// Add it to the list of sitefracs
					// +1 for a sitefraction
					//std::cout << "x[" << varcount << "] = (" << i->first << "," << std::distance(i->second.get_sublattice_iterator(),j) << "," << *k << ")" << std::endl;
					var_map.sitefrac_iters.resize(std::distance(phase_iter,i)+1);
					var_map.sitefrac_iters[std::distance(phase_iter,i)].resize(std::distance(i->second.get_sublattice_iterator(),j)+1);
					var_map.sitefrac_iters[std::distance(phase_iter,i)][std::distance(i->second.get_sublattice_iterator(),j)][*k] =
						std::make_pair(varcount,i);
					++varcount;
				}
			}
		}
	}
}

GibbsOpt::~GibbsOpt()
{}

bool GibbsOpt::get_nlp_info(Index& n, Index& m, Index& nnz_jac_g,
                         Index& nnz_h_lag, IndexStyleEnum& index_style)
{
	// TODO: fix this to use Boost multi_index (will probably require modification of sublattice_set)
  Index sublcount = 0; // total number of sublattices across all phases under consideration
  Index phasecount = 0; // total number of phases under consideration
  Index sitefraccount = 0; // total number of site fractions
  Index balancedsitefraccount = 0; // number of site fractions in sublattices with more than one species
  Index balanced_species_in_each_sublattice = 0; // number of site fractions subject to a mass balance constraint
  Index sitebalances = 0; // number of site fraction balance constraints
  Index speccount = (Index) conditions.xfrac.size() + 1; // total number of species for which mass must balance
  auto sitefrac_begin = var_map.sitefrac_iters.cbegin();

  for (auto i = var_map.sitefrac_iters.cbegin(); i != var_map.sitefrac_iters.cend(); ++i) {
	  const Phase_Collection::const_iterator cur_phase = var_map.phasefrac_iters.at(std::distance(sitefrac_begin,i)).get<2>();
	  const Sublattice_Collection::const_iterator subls_start = cur_phase->second.get_sublattice_iterator();
	  const Sublattice_Collection::const_iterator subls_end = cur_phase->second.get_sublattice_iterator_end();
	  sublcount += std::distance(subls_start,subls_end);
	  for (auto j = subls_start; j != subls_end;++j) {
		  int sublspeccount = 0;
		  for (auto k = (*j).get_species_iterator(); k != (*j).get_species_iterator_end();++k) {
			  // Check if this species in this sublattice is on our list of elements to investigate
			  if (std::find(conditions.elements.cbegin(),conditions.elements.cend(),*k) != conditions.elements.cend()) {
				  ++sitefraccount;
				  ++sublspeccount;
			  }
			  const auto balanced_spec_find = conditions.xfrac.find(*k);
			  const auto balanced_spec_end = conditions.xfrac.cend();
			  if (balanced_spec_find != balanced_spec_end) {
				  ++balanced_species_in_each_sublattice;
			  }
		  }
		  if (sublspeccount > 1) {
			  ++sitebalances;
			  balancedsitefraccount += sublspeccount;
		  }
	  }
	  ++phasecount;
  }
  // number of variables
  n = main_indices.size();
  //std::cout << "n = " << n << std::endl;
  // one phase fraction balance constraint (for multi-phase)
  // plus all the sublattice fraction balance constraints
  // plus all the mass balance constraints
  if (phasecount > 1) m = 1 + sitebalances + (speccount-1);
  else m = sitebalances + (speccount-1);
  //std::cout << "m = " << m << std::endl;

  // nonzeros in the jacobian of the lagrangian
  if (phasecount > 1) {
	  nnz_jac_g = phasecount + (speccount-1)*phasecount + balanced_species_in_each_sublattice + balancedsitefraccount;
  }
  else {
	  // single-phase case
	  nnz_jac_g = (speccount-1)*phasecount + balanced_species_in_each_sublattice + balancedsitefraccount;
  }
  //std::cout << "nnz_jac_g = " << nnz_jac_g << std::endl;

  index_style = C_STYLE;
  return true;
}

bool GibbsOpt::get_bounds_info(Index n, Number* x_l, Number* x_u,
                            Index m_num, Number* g_l, Number* g_u)
{
	for (Index i = 0; i < n; ++i) {
		x_l[i] = 0;
		x_u[i] = 1;
	}

	Index cons_index = 0;
	auto sitefrac_begin = var_map.sitefrac_iters.begin();
	auto sitefrac_end = var_map.sitefrac_iters.end();
	if (std::distance(sitefrac_begin, sitefrac_end) == 1) {
		// single phase optimization, fix the value of the phase fraction at 1
		x_l[0] = x_u[0] = 1;
		// no phase balance constraint needed
	}
	else {
		// enable the phase fraction balance constraint
		g_l[cons_index] = 0;
		g_u[cons_index] = 0;
		//std::cout << "set g_l,g_u[" << cons_index << "] = 0 (phase frac balance)" << std::endl;
		++cons_index;
	}
	for (auto i = sitefrac_begin; i != sitefrac_end; ++i) {
		const Phase_Collection::const_iterator cur_phase = var_map.phasefrac_iters.at(std::distance(sitefrac_begin,i)).get<2>();
		for (auto j = cur_phase->second.get_sublattice_iterator(); j != cur_phase->second.get_sublattice_iterator_end();++j) {
			// Site fraction balance constraint is disabled until we know the species count
			Index speccount = 0;
			// Iterating through the sublattice twice is not very efficient,
			// but we only set bounds once and this is simpler to read
			for (auto k = (*j).get_species_iterator(); k != (*j).get_species_iterator_end();++k) {
				// Check if this species in this sublattice is on our list of elements to investigate
				if (std::find(conditions.elements.cbegin(),conditions.elements.cend(),*k) != conditions.elements.cend()) {
					speccount = speccount + 1;
				}
			}
			if (speccount == 1) {
				// Only one species in this sublattice, fix its site fraction as 1
				for (auto k = (*j).get_species_iterator(); k != (*j).get_species_iterator_end();++k) {
					if (std::find(conditions.elements.cbegin(),conditions.elements.cend(),*k) != conditions.elements.cend()) {
						Index sitefracindex = var_map.sitefrac_iters[std::distance(sitefrac_begin,i)][std::distance(cur_phase->second.get_sublattice_iterator(),j)][*k].first;
						x_l[sitefracindex] = 1;
						x_u[sitefracindex] = 1;
						// no site balance constraint needed
					}
				}
			}
			else {
				// enable the site fraction balance constraint
				g_l[cons_index] = 0;
				g_u[cons_index] = 0;
				//std::cout << "set g_l,g_u[" << cons_index << "] = 0 (site frac balance)" << std::endl;
				++cons_index;
			}
		}
	}

	// Mass balance constraint
	for (auto i = 0; i < conditions.xfrac.size(); ++i) {
		g_l[cons_index] = 0;
		g_u[cons_index] = 0;
		//std::cout << "set g_l,g_u[" << cons_index << "] = 0 (mass balance)" << std::endl;
		++cons_index;
	}
	/*for (auto i = 0; i < m_num; ++i) {
		std::cout << "g_l[" << i << "] = " << g_l[i] << "; ";
		std::cout << "g_u[" << i << "] = " << g_u[i] << ";" << std::endl;
	}*/

	assert(m_num == cons_index);
	return true;
}

bool GibbsOpt::get_starting_point(Index n, bool init_x, Number* x,
	bool init_z, Number* z_L, Number* z_U,
	Index m, bool init_lambda,
	Number* lambda)
{
	const double numphases = var_map.phasefrac_iters.size();
	auto sitefrac_begin = var_map.sitefrac_iters.begin();
	double result = 0;
	int varcount = 0;
	// all phases
	for (auto i = sitefrac_begin; i != var_map.sitefrac_iters.end(); ++i) {
		const int phaseindex = var_map.phasefrac_iters.at(std::distance(sitefrac_begin,i)).get<0>();
		const Phase_Collection::const_iterator cur_phase = var_map.phasefrac_iters.at(std::distance(sitefrac_begin,i)).get<2>();
		x[phaseindex] = 1 / numphases; // phase fraction
		//std::cout << "x[" << phaseindex << "] = " << x[phaseindex] << std::endl;
		++varcount;
		for (auto j = cur_phase->second.get_sublattice_iterator(); j != cur_phase->second.get_sublattice_iterator_end();++j) {
			double speccount = 0;
			// Iterating through the sublattice twice is not very efficient,
			// but we only set the starting values once and this is far simpler to read
			for (auto k = (*j).get_species_iterator(); k != (*j).get_species_iterator_end();++k) {
				// Check if this species in this sublattice is on our list of elements to investigate
				if (std::find(conditions.elements.cbegin(),conditions.elements.cend(),*k) != conditions.elements.cend()) {
					speccount = speccount + 1;
				}
			}
			for (auto k = (*j).get_species_iterator(); k != (*j).get_species_iterator_end();++k) {
				// Check if this species in this sublattice is on our list of elements to investigate
				if (std::find(conditions.elements.cbegin(),conditions.elements.cend(),*k) != conditions.elements.cend()) {
					int sitefracindex = var_map.sitefrac_iters[std::distance(sitefrac_begin,i)][std::distance(cur_phase->second.get_sublattice_iterator(),j)][*k].first;
					x[sitefracindex] = 1 / speccount;
					//std::cout << "x[" << sitefracindex << "] = " << x[sitefracindex] << std::endl;
					++varcount;
				}
			}
		}
	}
	assert(varcount == m);
	return true;
}

bool GibbsOpt::eval_f(Index n, const Number* x, bool new_x, Number& obj_value)
{
	// return the value of the objective function
	//std::cout << "enter process_utree" << std::endl;
	obj_value = process_utree(master_tree, conditions, main_indices, (double*)x).get<double>();
	//std::cout << "exit process_utree" << std::endl;
	//std::cout << "eval_f: " << obj_value << " (new) == " << result << std::endl;
	return true;
}

bool GibbsOpt::eval_grad_f(Index n, const Number* x, bool new_x, Number* grad_f)
{
	// return the gradient of the objective function grad_{x} f(x)
	// calculate dF/dy(l,s,j)
	//std::cout << "eval_grad_f entered" << std::endl;
	for (auto i = main_indices.begin(); i != main_indices.end(); ++i) {
		double newgrad = differentiate_utree(master_tree, conditions, i->first, main_indices, (double*) x).get<double>();
		//std::cout << "grad_f[" << i->second << "]: " << newgrad <<  "(new) == " << grad_f[i->second] << std::endl;
		grad_f[i->second] = newgrad;
	}
	//std::cout << "eval_grad_f exit" << std::endl;
	return true;
}

bool GibbsOpt::eval_g(Index n, const Number* x, bool new_x, Index m_num, Number* g)
{
	//std::cout << "entering eval_g" << std::endl;
	// return the value of the constraints: g(x)
	double sum_phase_fracs = 0;
	Index cons_index = 0;
	sitefracs thesitefracs;
	const auto sitefrac_begin = var_map.sitefrac_iters.cbegin();
	const auto sitefrac_end = var_map.sitefrac_iters.cend();

	if (std::distance(sitefrac_begin,sitefrac_end) > 1) {
		// More than one phase
		// Preallocate g[0] for the phase fraction balance constraint
		// We will set it after we've calculated sum_phase_fracs
		++cons_index;
	}

	for (auto i = sitefrac_begin; i != sitefrac_end; ++i) {
		const Index phaseindex = var_map.phasefrac_iters.at(std::distance(sitefrac_begin,i)).get<0>();
		const Phase_Collection::const_iterator cur_phase = var_map.phasefrac_iters.at(std::distance(sitefrac_begin,i)).get<2>();
		const double fL = x[phaseindex]; // phase fraction
		const auto subl_begin = cur_phase->second.get_sublattice_iterator();
		const auto subl_end = cur_phase->second.get_sublattice_iterator_end();
		sublattice_vector subls_vec;
		//std::cout << "sum_phase_fracs += (x[" << phaseindex << "] = " << fL << ")" << std::endl;
		sum_phase_fracs += fL;

		for (auto j = subl_begin; j != subl_end; ++j) {
			double sum_site_fracs = 0;
			int speccount = 0;
			std::map<std::string,double> subl_map;
			for (auto k = (*j).get_species_iterator(); k != (*j).get_species_iterator_end();++k) {
				// Check if this species in this sublattice is on our list of elements to investigate
				if (std::find(conditions.elements.cbegin(),conditions.elements.cend(),*k) != conditions.elements.cend()) {
					Index sitefracindex = var_map.sitefrac_iters[std::distance(sitefrac_begin,i)][std::distance(cur_phase->second.get_sublattice_iterator(),j)][*k].first;
					subl_map[*k] = x[sitefracindex];
					////std::cout << "Sublattice " << std::distance(cur_phase->second.get_sublattice_iterator(),j) << std::endl;
					////std::cout << "subl_map[" << *k << "] = x[" << sitefracindex << "] = " << x[sitefracindex] << std::endl;
					sum_site_fracs  += subl_map[*k];
					++speccount;
				}
			}
			if (speccount > 1) {
				// More than one species in this sublattice
				// Site fraction balance constraint
				//std::cout << "g[" << cons_index << "] = " << sum_site_fracs << " - " << "1 = " << sum_site_fracs - 1 << std::endl;
				g[cons_index] = sum_site_fracs - 1;
				++cons_index;
			}
			subls_vec.push_back(subl_map);
		}
		thesitefracs.push_back(std::make_pair(cur_phase->first,subls_vec));

	}

	if (std::distance(sitefrac_begin,sitefrac_end) > 1) {
		// Phase fraction balance constraint
		//std::cout << "g[0] = " << sum_phase_fracs << " - 1 = " << sum_phase_fracs - 1 << std::endl;
		g[0] = sum_phase_fracs - 1;
	}

	// Mass balance constraint
	for (auto m = conditions.xfrac.cbegin(); m != conditions.xfrac.cend(); ++m) {
		double sumterm = 0;
		for (auto i = var_map.phasefrac_iters.begin(); i != var_map.phasefrac_iters.end(); ++i) {
			const Phase_Collection::const_iterator myphase = (*i).get<2>();
			sublattice_vector::const_iterator subls_start = (thesitefracs[std::distance(var_map.phasefrac_iters.begin(),i)].second).cbegin();
			sublattice_vector::const_iterator subls_end = (thesitefracs[std::distance(var_map.phasefrac_iters.begin(),i)].second).cend();
			double molefrac = mole_fraction(
				(*m).first,
				(*myphase).second.get_sublattice_iterator(),
				(*myphase).second.get_sublattice_iterator_end(),
				subls_start,
				subls_end
				);
			// sumterm += fL * molefrac
			////std::cout << "sumterm += (" << x[(*i).get<0>()] << " * " << molefrac << " = " << x[(*i).get<0>()] * molefrac << ")" << std::endl;
			sumterm += x[(*i).get<0>()] * molefrac;
		}
		// Mass balance constraint
		//std::cout << "g[" << cons_index << "] = " << sumterm << " - " << m->second << " = " << (sumterm - m->second) << std::endl;
		g[cons_index] = sumterm - m->second;
		++cons_index;
	}
	assert(cons_index == m_num);
	//std::cout << "exiting eval_g" << std::endl;
  return true;
}

bool GibbsOpt::eval_jac_g(Index n, const Number* x, bool new_x,
	Index m_num, Index nele_jac, Index* iRow, Index *jCol,
	Number* values)
{
	if (values == NULL) {
		//std::cout << "entering eval_jac_g values == NULL" << std::endl;
		Index cons_index = 0;
		Index jac_index = 0;
		const auto sitefrac_begin = var_map.sitefrac_iters.cbegin();
		const auto sitefrac_end = var_map.sitefrac_iters.cend();
		//sitefracs thesitefracs;
		for (auto i = sitefrac_begin; i != sitefrac_end; ++i) {
			const Index phaseindex = var_map.phasefrac_iters.at(std::distance(sitefrac_begin,i)).get<0>();
			const Phase_Collection::const_iterator cur_phase = var_map.phasefrac_iters.at(std::distance(sitefrac_begin,i)).get<2>();
			if (std::distance(sitefrac_begin,sitefrac_end) > 1) {
				// More than one phase
				// Phase fraction balance constraint
				//std::cout << "iRow[" << jac_index << "] = 0; jCol[" << jac_index << "] = " << phaseindex << std::endl;
				iRow[jac_index] = 0;
				jCol[jac_index] = phaseindex;
				//values[jac_index] = 1;
				++jac_index;
				if (cons_index == 0) ++cons_index;
			}

			//double sum_site_fracs = 0;
			//sublattice_vector subls_vec;
			for (auto j = cur_phase->second.get_sublattice_iterator(); j != cur_phase->second.get_sublattice_iterator_end();++j) {
				const auto spec_begin = (*j).get_species_iterator();
				const auto spec_end = (*j).get_species_iterator_end();
				int speccount = 0;
				//std::map<std::string,double> subl_map;

				for (auto k = spec_begin; k != spec_end; ++k) {
					// Check if this species in this sublattice is on our list of elements to investigate
					if (std::find(conditions.elements.cbegin(),conditions.elements.cend(),*k) != conditions.elements.cend()) {
						++speccount;
						Index sitefracindex = var_map.sitefrac_iters[std::distance(sitefrac_begin,i)][std::distance(cur_phase->second.get_sublattice_iterator(),j)][*k].first;
						//subl_map[*k] = x[sitefracindex];
						//sum_site_fracs += subl_map[*k];
					}
				}
				if (speccount > 1) {
					// More than one species in this sublattice
					// Add the site fraction balance constraint
					for (auto k = spec_begin; k != spec_end; ++k) {
						// Check if this species in this sublattice is on our list of elements to investigate
						if (std::find(conditions.elements.cbegin(),conditions.elements.cend(),*k) != conditions.elements.cend()) {
							// Site fraction balance constraint
							Index sitefracindex = var_map.sitefrac_iters[std::distance(sitefrac_begin,i)][std::distance(cur_phase->second.get_sublattice_iterator(),j)][*k].first;
							//std::cout << "iRow[" << jac_index << "] = " << cons_index << "; jCol[" << jac_index << "] = " << sitefracindex << std::endl;
							iRow[jac_index] = cons_index;
							jCol[jac_index] = sitefracindex;
							//values[jac_index] = 1;
							++jac_index;
						}
					}
					++cons_index;
				}
				//subls_vec.push_back(subl_map);
			}
			//thesitefracs.push_back(std::make_pair(cur_phase->first,subls_vec));
		}

		// Mass balance constraint
		for (auto m = conditions.xfrac.cbegin(); m != conditions.xfrac.cend(); ++m) {
			for (auto i = var_map.phasefrac_iters.begin(); i != var_map.phasefrac_iters.end(); ++i) {
				const Phase_Collection::const_iterator myphase = (*i).get<2>();
				sublattice_vector::const_iterator subls_start = (mysitefracs[std::distance(var_map.phasefrac_iters.begin(),i)].second).cbegin();
				sublattice_vector::const_iterator subls_end = (mysitefracs[std::distance(var_map.phasefrac_iters.begin(),i)].second).cend();
				const Index phaseindex = (*i).get<0>();
				//const double fL = x[phaseindex];
				/*double molefrac = mole_fraction(
					(*m),
					(*myphase).second.get_sublattice_iterator(),
					(*myphase).second.get_sublattice_iterator_end(),
					subls_start,
					subls_end
					);*/
				// Mass balance constraint, w.r.t phase fraction
				//std::cout << "iRow[" << jac_index << "] = " << cons_index << "; jCol[" << jac_index << "] = " << phaseindex << std::endl;
				iRow[jac_index] = cons_index;
				jCol[jac_index] = phaseindex;
				//values[jac_index] = molefrac;
				++jac_index;
			}
			auto sitefrac_begin = var_map.sitefrac_iters.begin();
			for (auto i = sitefrac_begin; i != var_map.sitefrac_iters.end(); ++i) {
				const Index phaseindex = var_map.phasefrac_iters.at(std::distance(sitefrac_begin,i)).get<0>();
				const Phase_Collection::const_iterator cur_phase = var_map.phasefrac_iters.at(std::distance(sitefrac_begin,i)).get<2>();
				//const double fL = x[phaseindex]; // phase fraction

				//sublattice_vector subls_vec;
				for (auto j = cur_phase->second.get_sublattice_iterator(); j != cur_phase->second.get_sublattice_iterator_end();++j) {
					//std::map<std::string,double> subl_map;
					int sublindex = std::distance(cur_phase->second.get_sublattice_iterator(),j);
					for (auto k = (*j).get_species_iterator(); k != (*j).get_species_iterator_end();++k) {
						// Check if this species in this sublattice is on our list of elements to investigate
						if ((*k) == (*m).first) {
							Index sitefracindex = var_map.sitefrac_iters[std::distance(sitefrac_begin,i)][sublindex][*k].first;
							//subl_map[*k] = x[sitefracindex];
							//std::cout << "iRow[" << jac_index << "] = " << cons_index << "; jCol[" << jac_index << "] = " << sitefracindex << std::endl;
							iRow[jac_index] = cons_index;
							jCol[jac_index] = sitefracindex;
							// values[jac_index] = fL * molefrac_deriv;
							++jac_index;
						}
					}
					//subls_vec.push_back(subl_map);
				}
			}
			// Mass balance
			++cons_index;
		}
		assert (cons_index == m_num);
	}
	else {
		Index cons_index = 0;
		Index jac_index = 0;
		const auto sitefrac_begin = var_map.sitefrac_iters.cbegin();
		const auto sitefrac_end = var_map.sitefrac_iters.cend();
		sitefracs thesitefracs;
		for (auto i = sitefrac_begin; i != sitefrac_end; ++i) {
			const Index phaseindex = var_map.phasefrac_iters.at(std::distance(sitefrac_begin,i)).get<0>();
			const Phase_Collection::const_iterator cur_phase = var_map.phasefrac_iters.at(std::distance(sitefrac_begin,i)).get<2>();
			double fL = x[phaseindex]; // phase fraction
			if (std::distance(sitefrac_begin,sitefrac_end) > 1) {
				// Phase fraction balance constraint
				//iRow[jac_index] = 0;
				//jCol[jac_index] = phaseindex;
				//std::cout << "jac_g values[" << jac_index << "] = 1" << std::endl;
				values[jac_index] = 1;
				++jac_index;
				if (cons_index == 0) ++cons_index;
			}

			sublattice_vector subls_vec;
			for (auto j = cur_phase->second.get_sublattice_iterator(); j != cur_phase->second.get_sublattice_iterator_end();++j) {
				std::map<std::string,double> subl_map;
				const auto spec_begin = (*j).get_species_iterator();
				const auto spec_end = (*j).get_species_iterator_end();
				int speccount = 0;
				for (auto k = spec_begin; k != spec_end; ++k) {
					// Check if this species in this sublattice is on our list of elements to investigate
					if (std::find(conditions.elements.cbegin(),conditions.elements.cend(),*k) != conditions.elements.cend()) {
						++speccount;
						Index sitefracindex = var_map.sitefrac_iters[std::distance(sitefrac_begin,i)][std::distance(cur_phase->second.get_sublattice_iterator(),j)][*k].first;
						subl_map[*k] = x[sitefracindex];
					}
				}
				if (speccount > 1) {
					// More than one species in this sublattice
					// Handle the site fraction balance constraints
					for (auto k = spec_begin; k != spec_end; ++k) {
						// Check if this species in this sublattice is on our list of elements to investigate
						if (std::find(conditions.elements.cbegin(),conditions.elements.cend(),*k) != conditions.elements.cend()) {
							// Site fraction balance constraint
							//iRow[jac_index] = cons_index;
							//jCol[jac_index] = sitefracindex;
							//std::cout << "jac_g values[" << jac_index << "] = 1" << std::endl;
							values[jac_index] = 1;
							++jac_index;
						}
					}
					++cons_index;
				}
				subls_vec.push_back(subl_map);
			}
			thesitefracs.push_back(std::make_pair(cur_phase->first,subls_vec));
		}

		// Mass balance constraint
		for (auto m = conditions.xfrac.cbegin(); m != conditions.xfrac.cend(); ++m) {
			for (auto i = var_map.phasefrac_iters.begin(); i != var_map.phasefrac_iters.end(); ++i) {
				const Phase_Collection::const_iterator myphase = (*i).get<2>();
				sublattice_vector::const_iterator subls_start = (thesitefracs[std::distance(var_map.phasefrac_iters.begin(),i)].second).cbegin();
				sublattice_vector::const_iterator subls_end = (thesitefracs[std::distance(var_map.phasefrac_iters.begin(),i)].second).cend();
				const Index phaseindex = (*i).get<0>();
				double fL = x[phaseindex];
				double molefrac = mole_fraction(
					m->first,
					(*myphase).second.get_sublattice_iterator(),
					(*myphase).second.get_sublattice_iterator_end(),
					subls_start,
					subls_end
					);
				// Mass balance constraint, w.r.t phase fraction
				//iRow[jac_index] = cons_index;
				//jCol[jac_index] = phaseindex;
				//std::cout << "jac_g values[" << jac_index << "] = " << molefrac << std::endl;
				values[jac_index] = molefrac;
				++jac_index;
			}
			auto sitefrac_begin = var_map.sitefrac_iters.begin();
			for (auto i = sitefrac_begin; i != var_map.sitefrac_iters.end(); ++i) {
				const Index phaseindex = var_map.phasefrac_iters.at(std::distance(sitefrac_begin,i)).get<0>();
				const Phase_Collection::const_iterator cur_phase = var_map.phasefrac_iters.at(std::distance(sitefrac_begin,i)).get<2>();
				//const Phase_Collection::const_iterator myphase = var_map.phasefrac_iters.at(std::distance(sitefrac_begin,i)).get<2>();
				sublattice_vector::const_iterator subls_start = (thesitefracs[std::distance(sitefrac_begin,i)].second).cbegin();
				sublattice_vector::const_iterator subls_end = (thesitefracs[std::distance(sitefrac_begin,i)].second).cend();
				double fL = x[phaseindex]; // phase fraction

				for (auto j = cur_phase->second.get_sublattice_iterator(); j != cur_phase->second.get_sublattice_iterator_end();++j) {
					int sublindex = std::distance(cur_phase->second.get_sublattice_iterator(),j);
					for (auto k = (*j).get_species_iterator(); k != (*j).get_species_iterator_end();++k) {
						// Check if this species in this sublattice is on our list of elements to investigate
						if ((*k) == (*m).first) {
							Index sitefracindex = var_map.sitefrac_iters[std::distance(sitefrac_begin,i)][sublindex][*k].first;
							//iRow[jac_index] = cons_index;
							//jCol[jac_index] = sitefracindex;
							double molefrac_deriv = mole_fraction_deriv(
								(*m).first,
								(*k),
								sublindex,
								cur_phase->second.get_sublattice_iterator(),
								cur_phase->second.get_sublattice_iterator_end(),
								subls_start,
								subls_end
								);
							//std::cout << "jac_g values[" << jac_index << "] = " << fL*molefrac_deriv << std::endl;
							values[jac_index] = fL * molefrac_deriv;
							++jac_index;
						}
					}
				}
			}
			// Mass balance
			++cons_index;
			//std::cout << "eval_jac_g: cons_index is now " << cons_index << std::endl;
		}
		//std::cout << "complete jac_index: " << jac_index << std::endl;
		assert(cons_index == m_num);
		//std::cout << "exit eval_jac_g with values" << std::endl;
	}
	//std::cout << "exiting eval_jac_g" << std::endl;
	return true;
}

bool GibbsOpt::eval_h(Index n, const Number* x, bool new_x,
                   Number obj_factor, Index m, const Number* lambda,
                   bool new_lambda, Index nele_hess, Index* iRow,
                   Index* jCol, Number* values)
{
	// No explicit evaluation of the Hessian
	return false;
}

void GibbsOpt::finalize_solution(SolverReturn status,
                              Index n, const Number* x, const Number* z_L, const Number* z_U,
                              Index m_num, const Number* g, const Number* lambda,
                              Number obj_value,
			      const IpoptData* ip_data,
			      IpoptCalculatedQuantities* ip_cq)
{
	auto sitefrac_begin = var_map.sitefrac_iters.begin();
	sitefracs thesitefracs;
	for (auto i = sitefrac_begin; i != var_map.sitefrac_iters.end(); ++i) {
		const Index phaseindex = var_map.phasefrac_iters.at(std::distance(sitefrac_begin,i)).get<0>();
		const Phase_Collection::const_iterator cur_phase = var_map.phasefrac_iters.at(std::distance(sitefrac_begin,i)).get<2>();
		const double fL = x[phaseindex]; // phase fraction

		constitution subls_vec;
		for (auto j = cur_phase->second.get_sublattice_iterator(); j != cur_phase->second.get_sublattice_iterator_end();++j) {
			std::unordered_map<std::string,double> subl_map;
			for (auto k = (*j).get_species_iterator(); k != (*j).get_species_iterator_end();++k) {
				// Check if this species in this sublattice is on our list of elements to investigate
				Index sitefracindex;
				if (std::find(conditions.elements.cbegin(),conditions.elements.cend(),*k) != conditions.elements.cend()) {
					sitefracindex = var_map.sitefrac_iters[std::distance(sitefrac_begin,i)][std::distance(cur_phase->second.get_sublattice_iterator(),j)][*k].first;
					subl_map[*k] = x[sitefracindex];
					//std::cout << "y(" << cur_phase->first << "," << *k << ") = " << x[sitefracindex] << std::endl;
				}
			}
			subls_vec.push_back(std::make_pair((*j).stoi_coef,subl_map));
		}
		//thesitefracs.push_back(std::make_pair(cur_phase->first,subls_vec));
		ph_map[cur_phase->first] = std::make_pair(fL,subls_vec);
	}
	/*for (auto m = conditions.elements.cbegin(); m != conditions.elements.cend(); ++m) {
		for (auto i = var_map.phasefrac_iters.begin(); i != var_map.phasefrac_iters.end(); ++i) {
			const Phase_Collection::const_iterator myphase = (*i).get<2>();
			sublattice_vector::const_iterator subls_start = (thesitefracs[std::distance(var_map.phasefrac_iters.begin(),i)].second).cbegin();
			sublattice_vector::const_iterator subls_end = (thesitefracs[std::distance(var_map.phasefrac_iters.begin(),i)].second).cend();
			const Index phaseindex = (*i).get<0>();
			const double fL = x[phaseindex];
			double molefrac = mole_fraction(
				(*m),
				(*myphase).second.get_sublattice_iterator(),
				(*myphase).second.get_sublattice_iterator_end(),
				subls_start,
				subls_end
				);
			//std::cout << "x(" << myphase->first << "," << *m << ") = " << molefrac << " ; X = " << fL*molefrac << std::endl;
		}
	}*/
	/*for (Index i = 0; i < m_num; ++i) {
		std::cout << "g[" << i << "] = " << g[i] << std::endl;
	}*/
}
