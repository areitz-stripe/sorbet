#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "common/typecase.h"
#include "core/lsp/QueryResponse.h"
#include "main/lsp/lsp.h"

using namespace std;

namespace sorbet::realmain::lsp {

UnorderedMap<core::NameRef, vector<core::SymbolRef>>
mergeMaps(UnorderedMap<core::NameRef, vector<core::SymbolRef>> &&first,
          UnorderedMap<core::NameRef, vector<core::SymbolRef>> &&second) {
    for (auto &other : second) {
        first[other.first].insert(first[other.first].end(), make_move_iterator(other.second.begin()),
                                  make_move_iterator(other.second.end()));
    }
    return std::move(first);
};

UnorderedMap<core::NameRef, vector<core::SymbolRef>> findSimilarMethodsIn(const core::GlobalState &gs,
                                                                          core::TypePtr receiver, string_view name) {
    UnorderedMap<core::NameRef, vector<core::SymbolRef>> result;
    typecase(
        receiver.get(),
        [&](core::ClassType *c) {
            const auto &owner = c->symbol.data(gs);
            for (auto member : owner->membersStableOrderSlow(gs)) {
                auto sym = member.second;
                if (sym.data(gs)->isMethod() && hasSimilarName(gs, sym.data(gs)->name, name)) {
                    result[sym.data(gs)->name].emplace_back(sym);
                }
            }
            for (auto mixin : owner->mixins()) {
                result = mergeMaps(std::move(result),
                                   findSimilarMethodsIn(gs, core::make_type<core::ClassType>(mixin), name));
            }
            if (owner->superClass().exists()) {
                result =
                    mergeMaps(std::move(result),
                              findSimilarMethodsIn(gs, core::make_type<core::ClassType>(owner->superClass()), name));
            }
        },
        [&](core::AndType *c) {
            result = mergeMaps(findSimilarMethodsIn(gs, c->left, name), findSimilarMethodsIn(gs, c->right, name));
        },
        [&](core::OrType *c) {
            auto lhs = findSimilarMethodsIn(gs, c->left, name);
            auto rhs = findSimilarMethodsIn(gs, c->right, name);
            for (auto it = rhs.begin(); it != rhs.end(); /*nothing*/) {
                auto &other = *it;
                auto fnd = lhs.find(other.first);
                if (fnd == lhs.end()) {
                    rhs.erase(it++);
                } else {
                    it->second.insert(it->second.end(), make_move_iterator(fnd->second.begin()),
                                      make_move_iterator(fnd->second.end()));
                    ++it;
                }
            }
        },
        [&](core::AppliedType *c) {
            result = findSimilarMethodsIn(gs, core::make_type<core::ClassType>(c->klass), name);
        },
        [&](core::ProxyType *c) { result = findSimilarMethodsIn(gs, c->underlying(), name); }, [&](core::Type *c) {});
    return result;
}

string methodSnippet(const core::GlobalState &gs, core::SymbolRef method) {
    auto shortName = method.data(gs)->name.data(gs)->shortName(gs);
    vector<string> typeAndArgNames;

    int i = 1;
    if (method.data(gs)->isMethod()) {
        for (auto &argSym : method.data(gs)->arguments()) {
            string s;
            if (argSym.flags.isBlock) {
                continue;
            }
            if (argSym.flags.isKeyword) {
                absl::StrAppend(&s, argSym.name.data(gs)->shortName(gs), ": ");
            }
            if (argSym.type) {
                absl::StrAppend(&s, "${", i++, ":", argSym.type->show(gs), "}");
            } else {
                absl::StrAppend(&s, "${", i++, "}");
            }
            typeAndArgNames.emplace_back(s);
        }
    }

    return fmt::format("{}({}){}", shortName, fmt::join(typeAndArgNames, ", "), "${0}");
}

optional<string> findDocumentation(string_view sourceCode, int beginIndex) {
    // everything in the file before the method definition.
    auto preDefinition = sourceCode.substr(0, sourceCode.rfind('\n', beginIndex));

    int lastNewlineLoc = preDefinition.rfind('\n');
    // if there is no '\n' in preDefinition, we're at the top of the file.
    if (lastNewlineLoc == preDefinition.npos) {
        return nullopt;
    }
    auto prevLine = preDefinition.substr(lastNewlineLoc, preDefinition.size() - lastNewlineLoc);
    if (prevLine.find('#') == prevLine.npos) {
        return nullopt;
    }

    string documentation = "";
    // keep looking for previous newline locations, searching for lines with # in them.
    while (prevLine.find('#') != prevLine.npos) {
        documentation = absl::StrCat(prevLine.substr(prevLine.find('#') + 1, prevLine.size()), "\n", documentation);
        int prevNewlineLoc = preDefinition.rfind('\n', lastNewlineLoc - 1);
        // if there is no '\n', we're at the top of the file, so just return documentation.
        if (prevNewlineLoc == preDefinition.npos) {
            break;
        }
        prevLine = preDefinition.substr(prevNewlineLoc, lastNewlineLoc - prevNewlineLoc);
        lastNewlineLoc = prevNewlineLoc;
    }
    if (documentation.empty()) {
        return nullopt;
    }
    return documentation;
}

unique_ptr<CompletionItem> LSPLoop::getCompletionItem(const core::GlobalState &gs, core::SymbolRef what,
                                                      core::TypePtr receiverType,
                                                      const unique_ptr<core::TypeConstraint> &constraint) {
    ENFORCE(what.exists());
    auto item = make_unique<CompletionItem>(string(what.data(gs)->name.data(gs)->shortName(gs)));
    auto resultType = what.data(gs)->resultType;
    if (!resultType) {
        resultType = core::Types::untypedUntracked();
    }
    if (what.data(gs)->isMethod()) {
        item->kind = CompletionItemKind::Function;
        if (what.exists()) {
            item->detail = methodDetail(gs, what, receiverType, nullptr, constraint);
        }
        if (clientCompletionItemSnippetSupport) {
            item->insertTextFormat = InsertTextFormat::Snippet;
            item->insertText = methodSnippet(gs, what);
        } else {
            item->insertTextFormat = InsertTextFormat::PlainText;
            item->insertText = string(what.data(gs)->name.data(gs)->shortName(gs));
        }

        optional<string> documentation = nullopt;
        if (what.data(gs)->loc().file().exists()) {
            documentation =
                findDocumentation(what.data(gs)->loc().file().data(gs).source(), what.data(gs)->loc().beginPos());
        }
        if (documentation) {
            if (documentation->find("@deprecated") != documentation->npos) {
                item->deprecated = true;
            }
            item->documentation = documentation;
        }
    } else if (what.data(gs)->isStaticField()) {
        item->kind = CompletionItemKind::Constant;
        item->detail = resultType->show(gs);
    } else if (what.data(gs)->isClass()) {
        item->kind = CompletionItemKind::Class;
    }
    return item;
}

void LSPLoop::findSimilarConstantOrIdent(const core::GlobalState &gs, const core::TypePtr receiverType,
                                         vector<unique_ptr<CompletionItem>> &items) {
    if (auto c = core::cast_type<core::ClassType>(receiverType.get())) {
        auto pattern = c->symbol.data(gs)->name.data(gs)->shortName(gs);
        logger->debug("Looking for constant similar to {}", pattern);
        core::SymbolRef owner = c->symbol;
        do {
            owner = owner.data(gs)->owner;
            for (auto member : owner.data(gs)->membersStableOrderSlow(gs)) {
                auto sym = member.second;
                if (sym.exists() && (sym.data(gs)->isClass() || sym.data(gs)->isStaticField()) &&
                    sym.data(gs)->name.data(gs)->kind == core::NameKind::CONSTANT &&
                    // hide singletons
                    hasSimilarName(gs, sym.data(gs)->name, pattern)) {
                    items.push_back(getCompletionItem(gs, sym, receiverType, nullptr));
                }
            }
        } while (owner != core::Symbols::root());
    }
}

LSPResult LSPLoop::handleTextDocumentCompletion(unique_ptr<core::GlobalState> gs, const MessageId &id,
                                                const CompletionParams &params) {
    auto response = make_unique<ResponseMessage>("2.0", id, LSPMethod::TextDocumentCompletion);
    if (!opts.lspAutocompleteEnabled) {
        response->error =
            make_unique<ResponseError>((int)LSPErrorCodes::InvalidRequest,
                                       "The `Autocomplete` LSP feature is experimental and disabled by default.");
        return LSPResult::make(move(gs), move(response));
    }

    prodCategoryCounterInc("lsp.messages.processed", "textDocument.completion");

    auto result = setupLSPQueryByLoc(move(gs), params.textDocument->uri, *params.position,
                                     LSPMethod::TextDocumentCompletion, false);

    if (auto run = get_if<TypecheckRun>(&result)) {
        gs = move(run->gs);
        auto &queryResponses = run->responses;
        vector<unique_ptr<CompletionItem>> items;
        if (!queryResponses.empty()) {
            auto resp = move(queryResponses[0]);

            if (auto sendResp = resp->isSend()) {
                auto pattern = sendResp->callerSideName.data(*gs)->shortName(*gs);
                auto receiverType = sendResp->dispatchResult->main.receiver;
                logger->debug("Looking for method similar to {}", pattern);
                UnorderedMap<core::NameRef, vector<core::SymbolRef>> methods =
                    findSimilarMethodsIn(*gs, receiverType, pattern);
                vector<pair<core::NameRef, vector<core::SymbolRef>>> methodsSorted;
                methodsSorted.insert(methodsSorted.begin(), make_move_iterator(methods.begin()),
                                     make_move_iterator(methods.end()));
                fast_sort(methodsSorted, [&](auto leftPair, auto rightPair) -> bool {
                    auto leftShortName = leftPair.first.data(*gs)->shortName(*gs);
                    auto rightShortName = rightPair.first.data(*gs)->shortName(*gs);
                    if (leftShortName != rightShortName) {
                        return leftShortName < rightShortName;
                    }
                    return leftPair.first._id < rightPair.first._id;
                });
                for (auto &entry : methodsSorted) {
                    if (entry.second[0].exists()) {
                        fast_sort(entry.second, [&](auto lhs, auto rhs) -> bool { return lhs._id < rhs._id; });
                        items.push_back(getCompletionItem(*gs, entry.second[0], receiverType,
                                                          sendResp->dispatchResult->main.constr));
                    }
                }
            } else if (auto identResp = resp->isIdent()) {
                findSimilarConstantOrIdent(*gs, identResp->retType.type, items);
            } else if (auto constantResp = resp->isConstant()) {
                findSimilarConstantOrIdent(*gs, constantResp->retType.type, items);
            }
        }
        response->result = make_unique<CompletionList>(false, move(items));
    } else if (auto error = get_if<pair<unique_ptr<ResponseError>, unique_ptr<core::GlobalState>>>(&result)) {
        // An error happened while setting up the query.
        response->error = move(error->first);
        gs = move(error->second);
    } else {
        // Should never happen, but satisfy the compiler.
        ENFORCE(false, "Internal error: setupLSPQueryByLoc returned invalid value.");
    }
    return LSPResult::make(move(gs), move(response));
}

} // namespace sorbet::realmain::lsp
