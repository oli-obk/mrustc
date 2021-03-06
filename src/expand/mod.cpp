/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * expand/mod.cpp
 * - Expand pass core code
 */
#include <ast/ast.hpp>
#include <ast/crate.hpp>
#include <main_bindings.hpp>
#include <synext.hpp>
#include <map>
#include "macro_rules.hpp"
#include "../parse/common.hpp"  // For reparse from macros
#include <ast/expr.hpp>
#include "cfg.hpp"

::std::map< ::std::string, ::std::unique_ptr<ExpandDecorator> >  g_decorators;
::std::map< ::std::string, ::std::unique_ptr<ExpandProcMacro> >  g_macros;

void Expand_Attrs(const ::AST::MetaItems& attrs, AttrStage stage,  ::std::function<void(const ExpandDecorator& d,const ::AST::MetaItem& a)> f);
void Expand_Mod(::AST::Crate& crate, LList<const AST::Module*> modstack, ::AST::Path modpath, ::AST::Module& mod, unsigned int first_item = 0);
void Expand_Expr(::AST::Crate& crate, LList<const AST::Module*> modstack, AST::Expr& node);
void Expand_Expr(::AST::Crate& crate, LList<const AST::Module*> modstack, ::std::shared_ptr<AST::ExprNode>& node);

void Register_Synext_Decorator(::std::string name, ::std::unique_ptr<ExpandDecorator> handler) {
    g_decorators[name] = mv$(handler);
}
void Register_Synext_Macro(::std::string name, ::std::unique_ptr<ExpandProcMacro> handler) {
    g_macros[name] = mv$(handler);
}


void ExpandDecorator::unexpected(const Span& sp, const AST::MetaItem& mi, const char* loc_str) const
{
    WARNING(sp, W0000, "Unexpected attribute " << mi.name() << " on " << loc_str);
}

void Expand_Attr(const Span& sp, const ::AST::MetaItem& a, AttrStage stage,  ::std::function<void(const Span& sp, const ExpandDecorator& d,const ::AST::MetaItem& a)> f)
{
    for( auto& d : g_decorators ) {
        if( d.first == a.name() ) {
            DEBUG("#[" << d.first << "] " << (int)d.second->stage() << "-" << (int)stage);
            if( d.second->stage() == stage ) {
                f(sp, *d.second, a);
            }
        }
    }
}
void Expand_Attrs(/*const */::AST::MetaItems& attrs, AttrStage stage,  ::std::function<void(const Span& sp, const ExpandDecorator& d,const ::AST::MetaItem& a)> f)
{
    for( auto& a : attrs.m_items )
    {
        if( a.name() == "cfg_attr" ) {
            if( check_cfg(attrs.m_span, a.items().at(0)) ) {
                auto inner_attr = mv$(a.items().at(1));
                Expand_Attr(attrs.m_span, inner_attr, stage, f);
                a = mv$(inner_attr);
            }
            else {
            }
        }
        else {
            Expand_Attr(attrs.m_span, a, stage, f);
        }
    }
}
void Expand_Attrs(::AST::MetaItems& attrs, AttrStage stage,  ::AST::Crate& crate, const ::AST::Path& path, ::AST::Module& mod, ::AST::Item& item)
{
    Expand_Attrs(attrs, stage,  [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate, path, mod, item); });
}
void Expand_Attrs(::AST::MetaItems& attrs, AttrStage stage,  ::AST::Crate& crate, ::AST::Module& mod, ::AST::ImplDef& impl)
{
    Expand_Attrs(attrs, stage,  [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate, mod, impl); });
}

::std::unique_ptr<TokenStream> Expand_Macro(
    const ::AST::Crate& crate, LList<const AST::Module*> modstack, ::AST::Module& mod,
    Span mi_span, const ::std::string& name, const ::std::string& input_ident, TokenTree& input_tt
    )
{
    if( name == "" ) {
        return ::std::unique_ptr<TokenStream>();
    }

    for( const auto& m : g_macros )
    {
        if( name == m.first )
        {
            auto e = m.second->expand(mi_span, crate, input_ident, input_tt, mod);
            return e;
        }
    }


    // Iterate up the module tree, using the first located macro
    for(const auto* ll = &modstack; ll; ll = ll->m_prev)
    {
        const auto& mac_mod = *ll->m_item;
        for( const auto& mr : mac_mod.macros() )
        {
            //DEBUG("- " << mr.name);
            if( mr.name == name )
            {
                if( input_ident != "" )
                    ERROR(mi_span, E0000, "macro_rules! macros can't take an ident");

                auto e = Macro_Invoke(name.c_str(), *mr.data, mv$(input_tt), mod);
                return e;
            }
        }
        // TODO: Shouldn't this use the _last_ located macro? Allowing later (local) defininitions to override it?
        for( const auto& mri : mac_mod.macro_imports_res() )
        {
            //DEBUG("- " << mri.name);
            if( mri.name == name )
            {
                if( input_ident != "" )
                    ERROR(mi_span, E0000, "macro_rules! macros can't take an ident");

                auto e = Macro_Invoke(name.c_str(), *mri.data, mv$(input_tt), mod);
                return e;
            }
        }
    }

    // Error - Unknown macro name
    ERROR(mi_span, E0000, "Unknown macro '" << name << "'");
}
::std::unique_ptr<TokenStream> Expand_Macro(const ::AST::Crate& crate, LList<const AST::Module*> modstack, ::AST::Module& mod, ::AST::MacroInvocation& mi)
{
    return Expand_Macro(crate, modstack, mod,  mi.span(), mi.name(), mi.input_ident(), mi.input_tt());
}

void Expand_Pattern(::AST::Crate& crate, LList<const AST::Module*> modstack, ::AST::Module& mod, ::AST::Pattern& pat, bool is_refutable)
{
    TU_MATCH(::AST::Pattern::Data, (pat.data()), (e),
    (MaybeBind,
        ),
    (Macro,
        const auto span = e.inv->span();

        auto tt = Expand_Macro(crate, modstack, mod,  *e.inv);
        if( ! tt ) {
            ERROR(span, E0000, "Macro in pattern didn't expand to anything");
        }
        auto& lex = *tt;
        auto newpat = Parse_Pattern(lex, is_refutable);
        if( LOOK_AHEAD(lex) != TOK_EOF ) {
            ERROR(span, E0000, "Trailing tokens in macro expansion");
        }

        if( pat.binding().is_valid() ) {
            if( newpat.binding().is_valid() )
                ERROR(span, E0000, "Macro expansion provided a binding, but one already present");
            newpat.binding() = mv$(pat.binding());
        }

        pat = mv$(newpat);
        ),
    (Any,
        ),
    (Box,
        Expand_Pattern(crate, modstack, mod,  *e.sub, is_refutable);
        ),
    (Ref,
        Expand_Pattern(crate, modstack, mod,  *e.sub, is_refutable);
        ),
    (Value,
        //Expand_Expr(crate, modstack, e.start);
        //Expand_Expr(crate, modstack, e.end);
        ),
    (Tuple,
        for(auto& sp : e.start)
            Expand_Pattern(crate, modstack, mod, sp, is_refutable);
        for(auto& sp : e.end)
            Expand_Pattern(crate, modstack, mod, sp, is_refutable);
        ),
    (StructTuple,
        for(auto& sp : e.tup_pat.start)
            Expand_Pattern(crate, modstack, mod, sp, is_refutable);
        for(auto& sp : e.tup_pat.end)
            Expand_Pattern(crate, modstack, mod, sp, is_refutable);
        ),
    (Struct,
        for(auto& sp : e.sub_patterns)
            Expand_Pattern(crate, modstack, mod, sp.second, is_refutable);
        ),
    (Slice,
        for(auto& sp : e.sub_pats)
            Expand_Pattern(crate, modstack, mod, sp, is_refutable);
        ),
    (SplitSlice,
        for(auto& sp : e.leading)
            Expand_Pattern(crate, modstack, mod, sp, is_refutable);
        for(auto& sp : e.trailing)
            Expand_Pattern(crate, modstack, mod, sp, is_refutable);
        )
    )
}

void Expand_Type(::AST::Crate& crate, LList<const AST::Module*> modstack, ::AST::Module& mod, ::TypeRef& ty)
{
    TU_MATCH(::TypeData, (ty.m_data), (e),
    (None,
        ),
    (Any,
        ),
    (Unit,
        ),
    (Bang,
        ),
    (Macro,
        auto tt = Expand_Macro(crate, modstack, mod,  e.inv);
        if(!tt)
            ERROR(e.inv.span(), E0000, "Macro invocation didn't yeild any data");
        auto new_ty = Parse_Type(*tt);
        if( tt->lookahead(0) != TOK_EOF )
            ERROR(e.inv.span(), E0000, "Extra tokens after parsed type");
        ty = mv$(new_ty);

        Expand_Type(crate, modstack, mod,  ty);
        ),
    (Primitive,
        ),
    (Function,
        Type_Function& tf = e.info;
        Expand_Type(crate, modstack, mod,  *tf.m_rettype);
        for(auto& st : tf.m_arg_types)
            Expand_Type(crate, modstack, mod,  st);
        ),
    (Tuple,
        for(auto& st : e.inner_types)
            Expand_Type(crate, modstack, mod,  st);
        ),
    (Borrow,
        Expand_Type(crate, modstack, mod,  *e.inner);
        ),
    (Pointer,
        Expand_Type(crate, modstack, mod,  *e.inner);
        ),
    (Array,
        Expand_Type(crate, modstack, mod,  *e.inner);
        if( e.size ) {
            Expand_Expr(crate, modstack,  e.size);
        }
        ),
    (Generic,
        ),
    (Path,
        ),
    (TraitObject,
        ),
    (ErasedType,
        // TODO: Visit paths.
        )
    )
}

struct CExpandExpr:
    public ::AST::NodeVisitor
{
    ::AST::Crate&    crate;
    LList<const AST::Module*>   modstack;
    ::std::unique_ptr<::AST::ExprNode> replacement;

    AST::ExprNode_Block*    current_block = nullptr;

    CExpandExpr(::AST::Crate& crate, LList<const AST::Module*> ms):
        crate(crate),
        modstack(ms)
    {
    }

    ::AST::Module& cur_mod() {
        return *const_cast< ::AST::Module*>(modstack.m_item);
    }

    void visit(::std::unique_ptr<AST::ExprNode>& cnode) {
        if(cnode.get())
            Expand_Attrs(cnode->attrs(), AttrStage::Pre,  [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, this->crate, cnode); });
        if(cnode.get())
        {
            cnode->visit(*this);
            // If the node was a macro, and it was consumed, reset it
            if( auto* n_mac = dynamic_cast<AST::ExprNode_Macro*>(cnode.get()) )
            {
                if( n_mac->m_name == "" )
                    cnode.reset();
            }
            if( this->replacement ) {
                cnode = mv$(this->replacement);
            }
        }

        if(cnode.get())
            Expand_Attrs(cnode->attrs(), AttrStage::Post,  [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, this->crate, cnode); });
        assert( ! this->replacement );
    }
    void visit_nodelete(const ::AST::ExprNode& parent, ::std::unique_ptr<AST::ExprNode>& cnode) {
        if( cnode.get() != nullptr )
        {
            this->visit(cnode);
            if(cnode.get() == nullptr)
                ERROR(parent.get_pos(), E0000, "#[cfg] not allowed in this position");
        }
        assert( ! this->replacement );
    }
    void visit_vector(::std::vector< ::std::unique_ptr<AST::ExprNode> >& cnodes) {
        for( auto& child : cnodes ) {
            this->visit(child);
        }
        // Delete null children
        for( auto it = cnodes.begin(); it != cnodes.end(); ) {
            if( it->get() == nullptr ) {
                it = cnodes.erase( it );
            }
            else {
                ++ it;
            }
        }
    }

    void visit(::AST::ExprNode_Macro& node) override
    {
        TRACE_FUNCTION_F("ExprNode_Macro - name = " << node.m_name);
        if( node.m_name == "" ) {
            return ;
        }

        auto& mod = this->cur_mod();
        auto ttl = Expand_Macro(
            crate, modstack, mod,
            Span(node.get_pos()),
            node.m_name, node.m_ident, node.m_tokens
            );
        if( ttl.get() != nullptr )
        {
            if( ttl->lookahead(0) != TOK_EOF )
            {
                SET_MODULE( (*ttl), mod );
                // Reparse as expression / item
                bool    add_silence_if_end = false;
                ::std::shared_ptr< AST::Module> tmp_local_mod;
                auto& local_mod_ptr = (this->current_block ? this->current_block->m_local_mod : tmp_local_mod);
                DEBUG("-- Parsing as expression line (legacy)");
                auto newexpr = Parse_ExprBlockLine_WithItems(*ttl, local_mod_ptr, add_silence_if_end);
                if( newexpr )
                {
                    // TODO: use add_silence_if_end - Applies if this node is the last node in the block.

                    // Then call visit on it again
                    DEBUG("--- Visiting new node");
                    this->visit(newexpr);
                    // And schedule it to replace the previous
                    replacement = mv$(newexpr);
                }
                else
                {
                    // TODO: Delete this node somehow? Or just leave it for later.
                }
                ASSERT_BUG(node.get_pos(), !tmp_local_mod, "TODO: Handle edge case where a macro expansion outside of a _Block creates an item");
            }
            DEBUG("ExprNode_Macro - replacement = " << replacement.get());
            node.m_name = "";
        }
    }

    void visit(::AST::ExprNode_Block& node) override {
        unsigned int mod_item_count = 0;
        // TODO: macro_rules! invocations within the expression list influence this.
        // > Solution: Defer creation of the local module until during expand.
        if( node.m_local_mod ) {
            Expand_Mod(crate, modstack, node.m_local_mod->path(), *node.m_local_mod);
            mod_item_count = node.m_local_mod->items().size();
        }

        auto saved = this->current_block;
        this->current_block = &node;
        this->visit_vector(node.m_nodes);
        this->current_block = saved;

        // HACK! Run Expand_Mod twice on local modules.
        if( node.m_local_mod ) {
            Expand_Mod(crate, modstack, node.m_local_mod->path(), *node.m_local_mod, mod_item_count);
        }
    }
    void visit(::AST::ExprNode_Asm& node) override {
        for(auto& v : node.m_output)
            this->visit_nodelete(node, v.value);
        for(auto& v : node.m_input)
            this->visit_nodelete(node, v.value);
    }
    void visit(::AST::ExprNode_Flow& node) override {
        this->visit_nodelete(node, node.m_value);
    }
    void visit(::AST::ExprNode_LetBinding& node) override {
        Expand_Type(crate, modstack, this->cur_mod(),  node.m_type);
        Expand_Pattern(crate, modstack, this->cur_mod(),  node.m_pat, false);
        this->visit_nodelete(node, node.m_value);
    }
    void visit(::AST::ExprNode_Assign& node) override {
        this->visit_nodelete(node, node.m_slot);
        this->visit_nodelete(node, node.m_value);
    }
    void visit(::AST::ExprNode_CallPath& node) override {
        this->visit_vector(node.m_args);
    }
    void visit(::AST::ExprNode_CallMethod& node) override {
        this->visit_nodelete(node, node.m_val);
        this->visit_vector(node.m_args);
    }
    void visit(::AST::ExprNode_CallObject& node) override {
        this->visit_nodelete(node, node.m_val);
        this->visit_vector(node.m_args);
    }
    void visit(::AST::ExprNode_Loop& node) override {
        this->visit_nodelete(node, node.m_cond);
        this->visit_nodelete(node, node.m_code);
        Expand_Pattern(crate, modstack, this->cur_mod(),  node.m_pattern, (node.m_type == ::AST::ExprNode_Loop::WHILELET));
        if(node.m_type == ::AST::ExprNode_Loop::FOR)
        {
            auto core_crate = (crate.m_load_std == ::AST::Crate::LOAD_NONE ? "" : "core");
            auto path_Some = ::AST::Path(core_crate, {::AST::PathNode("option"), ::AST::PathNode("Option"), ::AST::PathNode("Some")});
            auto path_None = ::AST::Path(core_crate, {::AST::PathNode("option"), ::AST::PathNode("Option"), ::AST::PathNode("None")});
            auto path_IntoIterator = ::AST::Path(core_crate, {::AST::PathNode("iter"), ::AST::PathNode("IntoIterator")});
            auto path_Iterator = ::AST::Path(core_crate, {::AST::PathNode("iter"), ::AST::PathNode("Iterator")});
            // Desugar into:
            // {
            //     match <_ as ::iter::IntoIterator>::into_iter(`m_cond`) {
            //     mut it => {
            //         `m_label`: loop {
            //             match ::iter::Iterator::next(&mut it) {
            //             Some(`m_pattern`) => `m_code`,
            //             None => break `m_label`,
            //             }
            //         }
            //     }
            // }
            ::std::vector< ::AST::ExprNode_Match_Arm>   arms;
            // - `Some(pattern ) => code`
            arms.push_back( ::AST::ExprNode_Match_Arm(
                ::make_vec1( ::AST::Pattern(::AST::Pattern::TagNamedTuple(), path_Some, ::make_vec1( mv$(node.m_pattern) ) ) ),
                nullptr,
                mv$(node.m_code)
                ) );
            // - `None => break label`
            arms.push_back( ::AST::ExprNode_Match_Arm(
                ::make_vec1( ::AST::Pattern(::AST::Pattern::TagValue(), ::AST::Pattern::Value::make_Named(path_None)) ),
                nullptr,
                ::AST::ExprNodeP(new ::AST::ExprNode_Flow(::AST::ExprNode_Flow::BREAK, node.m_label, nullptr))
                ) );

            replacement.reset(new ::AST::ExprNode_Match(
                ::AST::ExprNodeP(new ::AST::ExprNode_CallPath(
                    ::AST::Path(::AST::Path::TagUfcs(), ::TypeRef(node.span()), path_IntoIterator, { ::AST::PathNode("into_iter") } ),
                    ::make_vec1( mv$(node.m_cond) )
                    )),
                ::make_vec1(::AST::ExprNode_Match_Arm(
                    ::make_vec1( ::AST::Pattern(::AST::Pattern::TagBind(), "it") ),
                    nullptr,
                    ::AST::ExprNodeP(new ::AST::ExprNode_Loop(
                        node.m_label,
                        ::AST::ExprNodeP(new ::AST::ExprNode_Match(
                            ::AST::ExprNodeP(new ::AST::ExprNode_CallPath(
                                ::AST::Path(::AST::Path::TagUfcs(), ::TypeRef(node.span()), path_Iterator, { ::AST::PathNode("next") } ),
                                ::make_vec1( ::AST::ExprNodeP(new ::AST::ExprNode_UniOp(
                                    ::AST::ExprNode_UniOp::REFMUT,
                                    ::AST::ExprNodeP(new ::AST::ExprNode_NamedValue( ::AST::Path("it") ))
                                    )) )
                                )),
                            mv$(arms)
                            ))
                        )) )
                    )
                ) );
        }
    }
    void visit(::AST::ExprNode_Match& node) override {
        this->visit_nodelete(node, node.m_val);
        for(auto& arm : node.m_arms)
        {
            Expand_Attrs(arm.m_attrs, AttrStage::Pre ,  [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate,  arm); });
            if( arm.m_patterns.size() == 0 )
                continue ;
            for(auto& pat : arm.m_patterns) {
                Expand_Pattern(crate, modstack, this->cur_mod(),  pat, true);
            }
            this->visit_nodelete(node, arm.m_cond);
            this->visit_nodelete(node, arm.m_code);
            Expand_Attrs(arm.m_attrs, AttrStage::Post,  [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate,  arm); });
        }
        // Prune deleted arms
        for(auto it = node.m_arms.begin(); it != node.m_arms.end(); ) {
            if( it->m_patterns.size() == 0 )
                it = node.m_arms.erase(it);
            else
                ++ it;
        }
    }
    void visit(::AST::ExprNode_If& node) override {
        this->visit_nodelete(node, node.m_cond);
        this->visit_nodelete(node, node.m_true);
        this->visit_nodelete(node, node.m_false);
    }
    void visit(::AST::ExprNode_IfLet& node) override {
        Expand_Pattern(crate, modstack, this->cur_mod(),  node.m_pattern, true);
        this->visit_nodelete(node, node.m_value);
        this->visit_nodelete(node, node.m_true);
        this->visit_nodelete(node, node.m_false);
    }
    void visit(::AST::ExprNode_Integer& node) override { }
    void visit(::AST::ExprNode_Float& node) override { }
    void visit(::AST::ExprNode_Bool& node) override { }
    void visit(::AST::ExprNode_String& node) override { }
    void visit(::AST::ExprNode_ByteString& node) override { }
    void visit(::AST::ExprNode_Closure& node) override {
        for(auto& arg : node.m_args) {
            Expand_Pattern(crate, modstack, this->cur_mod(),  arg.first, false);
            Expand_Type(crate, modstack, this->cur_mod(),  arg.second);
        }
        Expand_Type(crate, modstack, this->cur_mod(),  node.m_return);
        this->visit_nodelete(node, node.m_code);
    }
    void visit(::AST::ExprNode_StructLiteral& node) override {
        this->visit_nodelete(node, node.m_base_value);
        for(auto& val : node.m_values)
        {
            // TODO: Attributes on struct literal items (#[cfg] only?)
            this->visit_nodelete(node, val.second);
        }
    }
    void visit(::AST::ExprNode_Array& node) override {
        this->visit_nodelete(node, node.m_size);
        this->visit_vector(node.m_values);
    }
    void visit(::AST::ExprNode_Tuple& node) override {
        this->visit_vector(node.m_values);
    }
    void visit(::AST::ExprNode_NamedValue& node) override { }
    void visit(::AST::ExprNode_Field& node) override {
        this->visit_nodelete(node, node.m_obj);
    }
    void visit(::AST::ExprNode_Index& node) override {
        this->visit_nodelete(node, node.m_obj);
        this->visit_nodelete(node, node.m_idx);
    }
    void visit(::AST::ExprNode_Deref& node) override {
        this->visit_nodelete(node, node.m_value);
    }
    void visit(::AST::ExprNode_Cast& node) override {
        this->visit_nodelete(node, node.m_value);
        Expand_Type(crate, modstack, this->cur_mod(),  node.m_type);
    }
    void visit(::AST::ExprNode_TypeAnnotation& node) override {
        this->visit_nodelete(node, node.m_value);
        Expand_Type(crate, modstack, this->cur_mod(),  node.m_type);
    }
    void visit(::AST::ExprNode_BinOp& node) override {
        this->visit_nodelete(node, node.m_left);
        this->visit_nodelete(node, node.m_right);
    }
    void visit(::AST::ExprNode_UniOp& node) override {
        this->visit_nodelete(node, node.m_value);
        // - Desugar question mark operator before resolve so it can create names
        if( node.m_type == ::AST::ExprNode_UniOp::QMARK ) {
            auto core_crate = (crate.m_load_std == ::AST::Crate::LOAD_NONE ? "" : "core");
            auto path_Ok  = ::AST::Path(core_crate, {::AST::PathNode("result"), ::AST::PathNode("Result"), ::AST::PathNode("Ok")});
            auto path_Err = ::AST::Path(core_crate, {::AST::PathNode("result"), ::AST::PathNode("Result"), ::AST::PathNode("Err")});
            auto path_From = ::AST::Path(core_crate, {::AST::PathNode("convert"), ::AST::PathNode("From")});
            path_From.nodes().back().args().m_types.push_back( ::TypeRef(node.span()) );

            // Desugars into
            // ```
            // match `m_value` {
            // Ok(v) => v,
            // Err(e) => return Err(From::from(e)),
            // }
            // ```

            ::std::vector< ::AST::ExprNode_Match_Arm>   arms;
            // `Ok(v) => v,`
            arms.push_back(::AST::ExprNode_Match_Arm(
                ::make_vec1( ::AST::Pattern(::AST::Pattern::TagNamedTuple(), path_Ok, ::make_vec1( ::AST::Pattern(::AST::Pattern::TagBind(), "v") )) ),
                nullptr,
                ::AST::ExprNodeP( new ::AST::ExprNode_NamedValue( ::AST::Path(::AST::Path::TagLocal(), "v") ) )
                ));
            // `Err(e) => return Err(From::from(e)),`
            arms.push_back(::AST::ExprNode_Match_Arm(
                ::make_vec1( ::AST::Pattern(::AST::Pattern::TagNamedTuple(), path_Err, ::make_vec1( ::AST::Pattern(::AST::Pattern::TagBind(), "e") )) ),
                nullptr,
                ::AST::ExprNodeP(new ::AST::ExprNode_Flow(
                    ::AST::ExprNode_Flow::RETURN,
                    "",
                    ::AST::ExprNodeP(new ::AST::ExprNode_CallPath(
                        ::AST::Path(path_Err),
                        ::make_vec1(
                            ::AST::ExprNodeP(new ::AST::ExprNode_CallPath(
                                ::AST::Path(::AST::Path::TagUfcs(), ::TypeRef(node.span()), mv$(path_From), { ::AST::PathNode("from") }),
                                ::make_vec1( ::AST::ExprNodeP( new ::AST::ExprNode_NamedValue( ::AST::Path(::AST::Path::TagLocal(), "e") ) ) )
                                ))
                            )
                        ))
                    ))
                ));

            replacement.reset(new ::AST::ExprNode_Match( mv$(node.m_value), mv$(arms) ));
        }
    }
};

void Expand_Expr(::AST::Crate& crate, LList<const AST::Module*> modstack, ::std::unique_ptr<AST::ExprNode>& node)
{
    auto visitor = CExpandExpr(crate, modstack);
    visitor.visit(node);
}
void Expand_Expr(::AST::Crate& crate, LList<const AST::Module*> modstack, ::std::shared_ptr<AST::ExprNode>& node)
{
    auto visitor = CExpandExpr(crate, modstack);
    node->visit(visitor);
    if( visitor.replacement ) {
        node.reset( visitor.replacement.release() );
    }
}
void Expand_Expr(::AST::Crate& crate, LList<const AST::Module*> modstack, AST::Expr& node)
{
    auto visitor = CExpandExpr(crate, modstack);
    node.visit_nodes(visitor);
    if( visitor.replacement ) {
        node = AST::Expr( mv$(visitor.replacement) );
    }
}

void Expand_BareExpr(const ::AST::Crate& crate, const AST::Module& mod, ::std::unique_ptr<AST::ExprNode>& node)
{
    Expand_Expr(const_cast< ::AST::Crate&>(crate), LList<const AST::Module*>(nullptr, &mod), node);
}

void Expand_Impl(::AST::Crate& crate, LList<const AST::Module*> modstack, ::AST::Path modpath, ::AST::Module& mod, ::AST::Impl& impl)
{
    Expand_Attrs(impl.def().attrs(), AttrStage::Pre,  crate, mod, impl.def());
    if( impl.def().type().is_wildcard() ) {
        DEBUG("Deleted");
        return ;
    }

    Expand_Type(crate, modstack, mod,  impl.def().type());
    //Expand_Type(crate, modstack, mod,  impl.def().trait());

    DEBUG("> Items");
    for( unsigned int idx = 0; idx < impl.items().size(); idx ++ )
    {
        auto& i = impl.items()[idx];
        DEBUG("  - " << i.name << " :: " << i.data->attrs);

        // TODO: Make a path from the impl definition? Requires having the impl def resolved to be correct
        // - Does it? the namespace is essentially the same. There may be issues with wherever the path is used though
        //::AST::Path path = modpath + i.name;

        auto attrs = mv$(i.data->attrs);
        Expand_Attrs(attrs, AttrStage::Pre,  crate, AST::Path(), mod, *i.data);

        TU_MATCH_DEF(AST::Item, (*i.data), (e),
        (
            throw ::std::runtime_error("BUG: Unknown item type in impl block");
            ),
        (None, ),
        (MacroInv,
            if( e.name() != "" )
            {
                TRACE_FUNCTION_F("Macro invoke " << e.name());
                // Move out of the module to avoid invalidation if a new macro invocation is added
                auto mi_owned = mv$(e);

                auto ttl = Expand_Macro(crate, modstack, mod, mi_owned);

                if( ttl.get() )
                {
                    // Re-parse tt
                    while( ttl->lookahead(0) != TOK_EOF )
                    {
                        Parse_Impl_Item(*ttl, impl);
                    }
                    // - Any new macro invocations ends up at the end of the list and handled
                }
                // Move back in (using the index, as the old pointr may be invalid)
                impl.items()[idx].data->as_MacroInv() = mv$(mi_owned);
            }
            ),
        (Function,
            for(auto& arg : e.args()) {
                Expand_Pattern(crate, modstack, mod,  arg.first, false);
                Expand_Type(crate, modstack, mod,  arg.second);
            }
            Expand_Type(crate, modstack, mod,  e.rettype());
            Expand_Expr(crate, modstack, e.code());
            ),
        (Static,
            Expand_Expr(crate, modstack, e.value());
            Expand_Type(crate, modstack, mod,  e.type());
            ),
        (Type,
            Expand_Type(crate, modstack, mod,  e.type());
            )
        )

        // Run post-expansion decorators and restore attributes
        {
            auto& i = impl.items()[idx];
            Expand_Attrs(attrs, AttrStage::Post,  crate, AST::Path(), mod, *i.data);
            // TODO: How would this be populated? It got moved out?
            if( i.data->attrs.m_items.size() == 0 )
                i.data->attrs = mv$(attrs);
        }
    }

    Expand_Attrs(impl.def().attrs(), AttrStage::Post,  crate, mod, impl.def());
}
void Expand_ImplDef(::AST::Crate& crate, LList<const AST::Module*> modstack, ::AST::Path modpath, ::AST::Module& mod, ::AST::ImplDef& impl_def)
{
    Expand_Attrs(impl_def.attrs(), AttrStage::Pre,  crate, mod, impl_def);
    if( impl_def.type().is_wildcard() ) {
        DEBUG("Deleted");
        return ;
    }

    Expand_Type(crate, modstack, mod,  impl_def.type());
    //Expand_Type(crate, modstack, mod,  impl_def.trait());

    Expand_Attrs(impl_def.attrs(), AttrStage::Post,  crate, mod, impl_def);
}

void Expand_Mod(::AST::Crate& crate, LList<const AST::Module*> modstack, ::AST::Path modpath, ::AST::Module& mod, unsigned int first_item)
{
    TRACE_FUNCTION_F("modpath = " << modpath);

    for( const auto& mi: mod.macro_imports_res() )
        DEBUG("- Imports '" << mi.name << "'");

    // Insert prelude if: Enabled for this module, present for the crate, and this module is not an anon
    if( crate.m_prelude_path != AST::Path() )
    {
        if( mod.m_insert_prelude && ! mod.is_anon() ) {
            mod.add_alias(false, ::AST::UseStmt(Span(), crate.m_prelude_path), "", {});
        }
    }

    DEBUG("Items");
    for( unsigned int idx = first_item; idx < mod.items().size(); idx ++ )
    {
        auto& i = mod.items()[idx];

        DEBUG("- " << i.name << " (" << ::AST::Item::tag_to_str(i.data.tag()) << ") :: " << i.data.attrs);
        ::AST::Path path = modpath + i.name;

        auto attrs = mv$(i.data.attrs);
        Expand_Attrs(attrs, AttrStage::Pre,  crate, path, mod, i.data);

        auto dat = mv$(i.data);

        TU_MATCH(::AST::Item, (dat), (e),
        (None,
            // Skip, nothing
            ),
        (MacroInv,
            // Move out of the module to avoid invalidation if a new macro invocation is added
            auto mi_owned = mv$(e);

            TRACE_FUNCTION_F("Macro invoke " << mi_owned.name());

            auto ttl = Expand_Macro(crate, modstack, mod, mi_owned);
            assert( mi_owned.name() != "");

            if( ttl.get() )
            {
                // Re-parse tt
                assert(ttl.get());
                DEBUG("-- Parsing as mod items");
                Parse_ModRoot_Items(*ttl, mod);
            }
            dat.as_MacroInv() = mv$(mi_owned);
            ),
        (Use,
            // No inner expand.
            ),
        (ExternBlock,
            // TODO: Run expand on inner items?
            // HACK: Just convert inner items into outer items
            auto items = mv$( e.items() );
            for(auto& i2 : items)
            {
                mod.items().push_back( mv$(i2) );
            }
            ),
        (Impl,
            Expand_Impl(crate, modstack, modpath, mod,  e);
            if( e.def().type().is_wildcard() ) {
                dat = AST::Item();
            }
            ),
        (NegImpl,
            Expand_ImplDef(crate, modstack, modpath, mod,  e);
            if( e.type().is_wildcard() ) {
                dat = AST::Item();
            }
            ),
        (Module,
            LList<const AST::Module*>   sub_modstack(&modstack, &e);
            Expand_Mod(crate, sub_modstack, path, e);
            ),
        (Crate,
            // Can't recurse into an `extern crate`
            ),

        (Struct,
            TU_MATCH(AST::StructData, (e.m_data), (sd),
            (Struct,
                for(auto it = sd.ents.begin(); it != sd.ents.end(); ) {
                    auto& si = *it;
                    Expand_Attrs(si.m_attrs, AttrStage::Pre, [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate, si); });
                    Expand_Type(crate, modstack, mod,  si.m_type);
                    Expand_Attrs(si.m_attrs, AttrStage::Post, [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate, si); });

                    if( si.m_name == "" )
                        it = sd.ents.erase(it);
                    else
                        ++it;
                }
                ),
            (Tuple,
                for(auto it = sd.ents.begin(); it != sd.ents.end(); ) {
                    auto& si = *it;
                    Expand_Attrs(si.m_attrs, AttrStage::Pre, [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate, si); });
                    Expand_Type(crate, modstack, mod,  si.m_type);
                    Expand_Attrs(si.m_attrs, AttrStage::Post, [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate, si); });

                    if( ! si.m_type.is_valid() )
                        it = sd.ents.erase(it);
                    else
                        ++it;
                }
                )
            )
            ),
        (Enum,
            for(auto& var : e.variants()) {
                Expand_Attrs(var.m_attrs, AttrStage::Pre,  [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate, var); });
                TU_MATCH(::AST::EnumVariantData, (var.m_data), (e),
                (Value,
                    Expand_Expr(crate, modstack,  e.m_value);
                    ),
                (Tuple,
                    for(auto& ty : e.m_sub_types) {
                        Expand_Type(crate, modstack, mod,  ty);
                    }
                    ),
                (Struct,
                    for(auto it = e.m_fields.begin(); it != e.m_fields.end(); ) {
                        auto& si = *it;
                        Expand_Attrs(si.m_attrs, AttrStage::Pre, [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate, si); });
                        Expand_Type(crate, modstack, mod,  si.m_type);
                        Expand_Attrs(si.m_attrs, AttrStage::Post, [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate, si); });

                        if( si.m_name == "" )
                            it = e.m_fields.erase(it);
                        else
                            ++it;
                    }
                    )
                )
                Expand_Attrs(var.m_attrs, AttrStage::Post,  [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate, var); });
            }
            ),
        (Union,
            for(auto it = e.m_variants.begin(); it != e.m_variants.end(); ) {
                auto& si = *it;
                Expand_Attrs(si.m_attrs, AttrStage::Pre, [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate, si); });
                Expand_Type(crate, modstack, mod,  si.m_type);
                Expand_Attrs(si.m_attrs, AttrStage::Post, [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate, si); });

                if( si.m_name == "" )
                    it = e.m_variants.erase(it);
                else
                    ++it;
            }
            ),
        (Trait,
            for(auto& ti : e.items())
            {
                auto attrs = mv$(ti.data.attrs);
                Expand_Attrs(attrs, AttrStage::Pre,  crate, AST::Path(), mod, ti.data);

                TU_MATCH_DEF(AST::Item, (ti.data), (e),
                (
                    throw ::std::runtime_error("BUG: Unknown item type in impl block");
                    ),
                (None, ),
                (Function,
                    for(auto& arg : e.args()) {
                        Expand_Pattern(crate, modstack, mod,  arg.first, false);
                        Expand_Type(crate, modstack, mod,  arg.second);
                    }
                    Expand_Type(crate, modstack, mod,  e.rettype());
                    Expand_Expr(crate, modstack, e.code());
                    ),
                (Static,
                    Expand_Expr(crate, modstack, e.value());
                    Expand_Type(crate, modstack, mod,  e.type());
                    ),
                (Type,
                    Expand_Type(crate, modstack, mod,  e.type());
                    )
                )

                Expand_Attrs(attrs, AttrStage::Post,  crate, AST::Path(), mod, ti.data);
                if( ti.data.attrs.m_items.size() == 0 )
                    ti.data.attrs = mv$(attrs);
            }
            ),
        (Type,
            Expand_Type(crate, modstack, mod,  e.type());
            ),

        (Function,
            for(auto& arg : e.args()) {
                Expand_Pattern(crate, modstack, mod,  arg.first, false);
                Expand_Type(crate, modstack, mod,  arg.second);
            }
            Expand_Type(crate, modstack, mod,  e.rettype());
            Expand_Expr(crate, modstack, e.code());
            ),
        (Static,
            Expand_Expr(crate, modstack, e.value());
            Expand_Type(crate, modstack, mod,  e.type());
            )
        )
        Expand_Attrs(attrs, AttrStage::Post,  crate, path, mod, dat);

        {

            auto& i = mod.items()[idx];
            if( i.data.tag() == ::AST::Item::TAGDEAD ) {
                i.data = mv$(dat);
            }
            // TODO: When would this _not_ be empty?
            if( i.data.attrs.m_items.size() == 0 )
                i.data.attrs = mv$(attrs);
        }
    }

    // IGNORE m_anon_modules, handled as part of expressions

    //for( const auto& mi: mod.macro_imports_res() )
    //    DEBUG("- Imports '" << mi.name << "'");
}
void Expand_Mod_IndexAnon(::AST::Crate& crate, ::AST::Module& mod)
{
    TRACE_FUNCTION_F("mod=" << mod.path());

    for(auto& i : mod.items())
    {
        DEBUG("- " << i.data.tag_str() << " '" << i.name << "'");
        TU_IFLET(::AST::Item, (i.data), Module, e,
            Expand_Mod_IndexAnon(crate, e);

            // TODO: Also ensure that all #[macro_export] macros end up in parent
        )
    }

    for( auto& mp : mod.anon_mods() )
    {
        if( mp.unique() ) {
            DEBUG("- " << mp->path() << " dropped due to node destruction");
            mp.reset();
        }
        else {
            Expand_Mod_IndexAnon(crate, *mp);
        }
    }
}
void Expand(::AST::Crate& crate)
{
    auto modstack = LList<const ::AST::Module*>(nullptr, &crate.m_root_module);

    // 1. Crate attributes
    Expand_Attrs(crate.m_attrs, AttrStage::Pre,  [&](const auto& sp, const auto& d, const auto& a){ d.handle(sp, a, crate); });

    // Insert magic for libstd/libcore
    // NOTE: The actual crates are loaded in "LoadCrates" using magic in AST::Crate::load_externs
    switch( crate.m_load_std )
    {
    case ::AST::Crate::LOAD_STD:
        if( crate.m_prelude_path == AST::Path() )
            crate.m_prelude_path = AST::Path("std", {AST::PathNode("prelude"), AST::PathNode("v1")});
        crate.m_extern_crates.at("std").with_all_macros([&](const auto& name, const auto& mac) {
            crate.m_root_module.add_macro_import( name, mac );
            });
        crate.m_root_module.add_ext_crate(false, "std", "std", ::AST::MetaItems {});
        break;
    case ::AST::Crate::LOAD_CORE:
        if( crate.m_prelude_path == AST::Path() )
            crate.m_prelude_path = AST::Path("core", {AST::PathNode("prelude"), AST::PathNode("v1")});
        crate.m_extern_crates.at("core").with_all_macros([&](const auto& name, const auto& mac) {
            crate.m_root_module.add_macro_import( name, mac );
            });
        crate.m_root_module.add_ext_crate(false, "core", "core", ::AST::MetaItems {});
        break;
    case ::AST::Crate::LOAD_NONE:
        break;
    }

    // 2. Module attributes
    for( auto& a : crate.m_attrs.m_items )
    {
        for( auto& d : g_decorators ) {
            if( d.first == a.name() && d.second->stage() == AttrStage::Pre ) {
                //d.second->handle(a, crate, ::AST::Path(), crate.m_root_module, crate.m_root_module);
            }
        }
    }

    // 3. Module tree
    Expand_Mod(crate, modstack, ::AST::Path("",{}), crate.m_root_module);

    // Post-process
    Expand_Mod_IndexAnon(crate, crate.m_root_module);
}


