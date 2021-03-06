/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * trans/main_bindings.hpp
 * - Trans functions called by main()
 */
#pragma once

#include "trans_list.hpp"

namespace HIR {
class Crate;
}

struct TransOptions
{
    unsigned int opt_level = 0;
    bool emit_debug_info = false;

    ::std::vector< ::std::string>   library_search_dirs;
    ::std::vector< ::std::string>   libraries;
};

extern TransList Trans_Enumerate_Main(const ::HIR::Crate& crate);
// NOTE: This also sets the saveout flags
extern TransList Trans_Enumerate_Public(::HIR::Crate& crate);

extern void Trans_Codegen(const ::std::string& outfile, const TransOptions& opt, const ::HIR::Crate& crate, const TransList& list, bool is_executable);
