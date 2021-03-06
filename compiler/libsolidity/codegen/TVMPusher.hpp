/*
 * Copyright 2018-2019 TON DEV SOLUTIONS LTD.
 *
 * Licensed under the  terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License.
 *
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the  GNU General Public License for more details at: https://www.gnu.org/licenses/gpl-3.0.html
 */
/**
 * @author TON Labs <connect@tonlabs.io>
 * @date 2019
 */

#pragma once

#include <map>

#include <liblangutil/ErrorReporter.h>
#include <liblangutil/Exceptions.h>
#include <liblangutil/SourceReferenceFormatter.h>
#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>

#include "TVMCommons.hpp"
#include "TVMConstants.hpp"
#include "TVMABI.hpp"

using namespace std;
using namespace solidity;
using namespace solidity::frontend;
using namespace langutil;
using namespace solidity::util;

namespace solidity::frontend {

class StructCompiler;

class DictOperation {
public:
	DictOperation(StackPusherHelper& pusher, Type const& keyType, Type const& valueType, ASTNode const& node);
protected:
	virtual void doDictOperation() final;
	virtual void onCell() = 0;
	virtual void onSmallStruct() = 0;
	virtual void onLargeStruct() = 0;
	virtual void onAddress() = 0;
	virtual void onByteArrayOrString() = 0;
	virtual void onIntegralOrArrayOrVarInt() = 0;
	virtual void onMapOrECC() = 0;
protected:
	StackPusherHelper& pusher;
	Type const& keyType;
	const int keyLength{};
	Type const& valueType;
	const Type::Category valueCategory{};
	ASTNode const& node;
};

class TVMStack {
	int m_size{};
	// map parameters or local variables to their absolute stack position
	std::map<Declaration const*, int> m_params;

public:
	int size() const;
	void change(int diff);
	bool isParam(Declaration const* name) const;
	void add(Declaration const* name, bool doAllocation);
	int getOffset(Declaration const* name) const;
	int getOffset(int stackPos) const;
	int getStackSize(Declaration const* name) const;
	void ensureSize(int savedStackSize, const string& location) const;
};

struct CodeLines {
	vector<string> lines;
	int tabQty{};

	string str(const string& indent = "") const;
	void addTabs(const int qty = 1);
	void subTabs(const int qty = 1);
	void startContinuation();
	void endContinuation();
	void push(const string& cmd);
	void append(const CodeLines& oth);
};

class TVMCompilerContext {
	const ContractDefinition*				m_contract = nullptr;
	string_map<const FunctionDefinition*>	m_functions;
	map<const FunctionDefinition*, const ContractDefinition*>	m_function2contract;

	bool haveFallback = false;
	bool haveOnBounce = false;
	bool haveReceive = false;
	bool ignoreIntOverflow = false;
	bool m_haveOffChainConstructor = false;
	PragmaDirectiveHelper const& m_pragmaHelper;
	std::map<VariableDeclaration const *, int> m_stateVarIndex;

	void addFunction(FunctionDefinition const* _function);
	void initMembers(ContractDefinition const* contract);

public:
	TVMCompilerContext(ContractDefinition const* contract, PragmaDirectiveHelper const& pragmaHelper);

	FunctionDefinition const* m_currentFunction = nullptr;
	map<string, CodeLines> m_inlinedFunctions;

	int getStateVarIndex(VariableDeclaration const *variable) const;
	std::vector<VariableDeclaration const *> notConstantStateVariables() const;
	PragmaDirectiveHelper const& pragmaHelper() const;
	bool haveTimeInAbiHeader() const;
	bool isStdlib() const;
	string getFunctionInternalName(FunctionDefinition const* _function) const;
	static string getFunctionExternalName(FunctionDefinition const* _function);
	bool isPureFunction(FunctionDefinition const* f) const;
	const ContractDefinition* getContract() const;
	const ContractDefinition* getContract(const FunctionDefinition* f) const;
	const FunctionDefinition* getLocalFunction(const string& fname) const;
	bool haveFallbackFunction() const;
	bool haveReceiveFunction() const;
	bool haveOnBounceHandler() const;
	bool ignoreIntegerOverflow() const;
	bool haveOffChainConstructor() const;
	FunctionDefinition const* afterSignatureCheck() const;
	bool storeTimestampInC4() const;
};

class StackPusherHelper {
protected:
	TVMStack m_stack;
	CodeLines m_code;
	const TVMCompilerContext* const m_ctx;
	StructCompiler* m_structCompiler;

public:
	explicit StackPusherHelper(const TVMCompilerContext* ctx, const int stackSize = 0);
	void tryPollLastRetOpcode();
	void pollLastOpcode();
	void append(const CodeLines& oth);
	void addTabs(const int qty = 1);
	void subTabs(const int qty = 1);
	void pushCont(const CodeLines& cont, const string& comment = "");
	void generateGlobl(const string& fname, const bool isPublic);
	void generateInternal(const string& fname, const int id);
	void generateMacro(const string& functionName);
	CodeLines code() const;

	[[nodiscard]]
	const TVMCompilerContext& ctx() const;
	void push(int stackDiff, const string& cmd);
	void startContinuation();
	void endContinuation();
	StructCompiler& structCompiler();
	TVMStack& getStack();
	void pushLog(const std::string& str);
	void pushLines(const std::string& lines);
	void untuple(int n);
	void index(int index);
	void set_index(int index);
	void tuple(int qty);
	void resetAllStateVars();
	void getGlob(VariableDeclaration const * vd);
	void getGlob(int index);
	void setGlob(int index);
	void setGlob(VariableDeclaration const * vd);
	void pushS(int i);
	void pushInt(int i);
	void stzeroes(int qty);
	void stones(int qty);
	void sendrawmsg();
	void loadArray(bool directOrder = true);
	void preLoadArray();
	void load(const Type* type);
	void preload(const Type* type);
	void pushZeroAddress();
	void generateC7ToT4Macro();

	static void addBinaryNumberToString(std::string &s, u256 value, int bitlen = 256);
	static std::string binaryStringToSlice(const std::string & s);
	static std::string gramsToBinaryString(Literal const* literal);
	static std::string gramsToBinaryString(u256 value);
	std::string literalToSliceAddress(Literal const* literal, bool pushSlice = true);

	bool tryImplicitConvert(Type const *leftType, Type const *rightType);
	void push(const CodeLines& codeLines);
	void pushPrivateFunctionOrMacroCall(const int stackDelta, const string& fname);
	void pushCall(const string& functionName, const FunctionType* ft);
	void drop(int cnt);
	void blockSwap(int m, int n);
	void reverse(int i, int j);
	void dropUnder(int leftCount, int droppedCount);
	void exchange(int i, int j);
	static void checkThatKeyCanBeRestored(Type const* keyType, ASTNode const& node);
	void prepareKeyForDictOperations(Type const* key);
	[[nodiscard]]
	std::pair<std::string, int> int_msg_info(const std::set<int> &isParamOnStack, const std::map<int, std::string> &constParams);
	[[nodiscard]]
	std::pair<std::string, int> ext_msg_info(const std::set<int> &isParamOnStack);
	void appendToBuilder(const std::string& bitString);

	[[nodiscard]]
	// return true if result is a builder
	bool prepareValueForDictOperations(Type const* keyType, Type const* dictValueType, bool isValueBuilder);
	static TypePointer parseIndexType(Type const* type);
	static TypePointer parseValueType(IndexAccess const& indexAccess);

	enum class SetDictOperation { Set, Replace, Add };
	void setDict(Type const &keyType, Type const &valueType, bool isValueBuilder, ASTNode const& node, SetDictOperation opcode = SetDictOperation::Set);

	bool tryAssignParam(Declaration const* name);

	enum class GetDictOperation {
		GetFromMapping,
		GetSetFromMapping,
		GetAddFromMapping,
		GetReplaceFromMapping,
		GetFromArray,
		Fetch,
		Exist
	};
	void getDict(const Type& keyType, const Type& valueType, ASTNode const& node, const GetDictOperation op,
	             const bool resultAsSliceForStruct);

	void ensureValueFitsType(const ElementaryTypeNameToken& typeName, const ASTNode& node);

	void pushDefaultValue(Type const* type, bool isResultBuilder = false);
	void sendIntMsg(const std::map<int, const Expression *> &exprs,
					const std::map<int, std::string> &constParams,
					const std::function<void(int)> &appendBody,
					const std::function<void()> &pushSendrawmsgFlag);
	void sendMsg(const std::set<int>& isParamOnStack,
				 const std::map<int, std::string> &constParams,
				 const std::function<void(int)> &appendBody,
				 const std::function<void()> &appendStateInit,
				 const std::function<void()> &pushSendrawmsgFlag,
				 bool isInternalMessage = true);
};

CodeLines switchSelectorIfNeed(FunctionDefinition const* f);

} // end solidity::frontend
