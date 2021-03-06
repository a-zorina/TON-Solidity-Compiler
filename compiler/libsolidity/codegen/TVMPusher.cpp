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

#include "TVMCommons.hpp"
#include "TVMPusher.hpp"
#include "TVMContractCompiler.hpp"
#include "TVMExpressionCompiler.hpp"
#include "TVMABI.hpp"

using namespace solidity::frontend;

DictOperation::DictOperation(StackPusherHelper& pusher, Type const& keyType, Type const& valueType, ASTNode const& node) :
		pusher{pusher},
		keyType{keyType},
		keyLength{lengthOfDictKey(&keyType)},
		valueType{valueType},
		valueCategory{valueType.category()},
		node{node} {
}

void DictOperation::doDictOperation() {
	if (valueCategory == Type::Category::TvmCell) {
		onCell();
	} else if (valueCategory == Type::Category::Struct) {
		if (StructCompiler::isCompatibleWithSDK(keyLength, to<StructType>(&valueType))) {
			onSmallStruct();
		} else {
			onLargeStruct();
		}
	} else if (isIn(valueCategory, Type::Category::Address, Type::Category::Contract)) {
		onAddress();
	} else if (isByteArrayOrString(&valueType)) {
		onByteArrayOrString();
	} else if (isIntegralType(&valueType) || isUsualArray(&valueType) || valueCategory == Type::Category::VarInteger) {
		onIntegralOrArrayOrVarInt();
	} else if (isIn(valueCategory, Type::Category::Mapping, Type::Category::ExtraCurrencyCollection)) {
		onMapOrECC();
	} else {
		cast_error(node, "Unsupported value type: " + valueType.toString());
	}
}

StackPusherHelper::StackPusherHelper(const TVMCompilerContext *ctx, const int stackSize) :
		m_ctx(ctx),
		m_structCompiler{new StructCompiler{this,
											ctx->notConstantStateVariables(),
											256 + (m_ctx->storeTimestampInC4()? 64 : 0) + 1, // pubkey + timestamp + constructor_flag
											true}} {
	m_stack.change(stackSize);
}

void StackPusherHelper::pushLog(const std::string& str) {
	if (!TVMContractCompiler::g_without_logstr) {
		push(0, "PRINTSTR " + str);
	}
}

StructCompiler &StackPusherHelper::structCompiler() {
	return *m_structCompiler;
}

void StackPusherHelper::generateC7ToT4Macro() {
	pushLines(R"(
.macro	c7_to_c4
GETGLOB 2
NEWC
STU 256
)");
	if (ctx().storeTimestampInC4()) {
		pushLines(R"(
GETGLOB 3
STUR 64
)");
	}
	pushLines(R"(
GETGLOB 6
STUR 1
)");
	if (!ctx().notConstantStateVariables().empty()) {
		structCompiler().stateVarsToBuilder();
	}
	pushLines(R"(
ENDC
POP C4
)");
	push(0, " ");
}

bool
StackPusherHelper::prepareValueForDictOperations(Type const *keyType, Type const *dictValueType, bool isValueBuilder) {
	// value
	if (isIntegralType(dictValueType)) {
		if (!isValueBuilder) {
			push(0, "NEWC");
			push(0, storeIntegralOrAddress(dictValueType, false));
			return true;
		}
	} else if (dictValueType->category() == Type::Category::Struct) {
		if (StructCompiler::isCompatibleWithSDK(lengthOfDictKey(keyType), to<StructType>(dictValueType))) {
			if (isValueBuilder) {
				return true;
			} else {
				StructCompiler sc{this, to<StructType>(dictValueType)};
				sc.tupleToBuilder();
				return true;
			}
		} else {
			if (!isValueBuilder) {
				StructCompiler sc{this, to<StructType>(dictValueType)};
				sc.tupleToBuilder();
			}
			push(0, "ENDC");
			return true; //NOTE: it's not builder. It's cell
		}
	} else if (isUsualArray(dictValueType)) {
		if (!isValueBuilder) {
			push(-1 + 2, "UNPAIR"); // size dict
			push(0, "SWAP"); // dict size
			push(+1, "NEWC"); // dict size builder
			push(-1, "STU 32"); // dict builder
			push(-1, "STDICT"); // builder
			return true;
		}
	} else if (to<TvmCellType>(dictValueType) || (to<ArrayType>(dictValueType) && to<ArrayType>(dictValueType)->isByteArray())) {
		if (isValueBuilder) {
			push(0, "ENDC");
			return false;
		}
	} else if (isIn(dictValueType->category(), Type::Category::Mapping, Type::Category::ExtraCurrencyCollection)) {
		if (!isValueBuilder) {
			push(+1, "NEWC"); // dict builder
			push(-1, "STDICT"); // builder
			return true;
		}
	} else if (dictValueType->category() == Type::Category::VarInteger) {
		if (!isValueBuilder) {
			push(+1, "NEWC"); // value builder
			push(0, "SWAP"); // builder value
			push(-1, "STVARUINT32"); // builder
			return true;
		}
	}

	return isValueBuilder;
}

class DictSet : public DictOperation {
public:
	DictSet(StackPusherHelper& pusher, Type const &keyType, Type const &valueType, bool isValueBuilder, ASTNode const &node,
	        StackPusherHelper::SetDictOperation operation)  :
		DictOperation{pusher, keyType, valueType, node},
		isValueBuilder{isValueBuilder},
		operation{operation} {

	}

	void dictSet() {
		// stack: value key dict
		int keyLength = lengthOfDictKey(&keyType);
		pusher.pushInt(keyLength);
		// stack: value index dict keyBitLength
		opcode = "DICT" + typeToDictChar(&keyType);
		switch (operation) {
			case StackPusherHelper::SetDictOperation::Set:
				opcode += "SET";
				break;
			case StackPusherHelper::SetDictOperation::Replace:
				opcode += "REPLACE";
				break;
			case StackPusherHelper::SetDictOperation::Add:
				opcode += "ADD";
				break;
		}
		doDictOperation();
		switch (operation) {
			case StackPusherHelper::SetDictOperation::Set:
				pusher.push(-4 + 1, opcode);
				break;
			case StackPusherHelper::SetDictOperation::Replace:
			case StackPusherHelper::SetDictOperation::Add:
				pusher.push(-4 + 2, opcode);
				break;
		}
	}

protected:
	void onCell() override {
		solAssert(!isValueBuilder, "");
		opcode += "REF";
	}

	void onSmallStruct() override {
		solAssert(isValueBuilder, "");
		opcode += "B";
	}

	void onLargeStruct() override {
		solAssert(isValueBuilder, "");
		opcode += "REF";
	}

	void onByteArrayOrString() override {
		solAssert(!isValueBuilder, "");
		opcode += "REF";
	}

	void onAddress() override {
		if (isValueBuilder) {
			opcode += "B";
		}
	}

	void onIntegralOrArrayOrVarInt() override {
		solAssert(isValueBuilder, "");
		opcode += "B";
	}

	void onMapOrECC() override {
		solAssert(isValueBuilder, "");
		opcode += "B";
	}

private:
	bool isValueBuilder;
	StackPusherHelper::SetDictOperation operation;
	std::string opcode;
};

void StackPusherHelper::setDict(Type const &keyType, Type const &valueType, bool isValueBuilder, ASTNode const &node,
                                SetDictOperation operation) {
	DictSet d{*this, keyType, valueType, isValueBuilder, node, operation};
	d.dictSet();
}

void StackPusherHelper::tryPollLastRetOpcode() {
	if (m_code.lines.empty()) {
		return;
	}
	if (std::regex_match(m_code.lines.back(), std::regex("(\t*)RET"))) {
		m_code.lines.pop_back();
	}
}

void StackPusherHelper::pollLastOpcode() {
	m_code.lines.pop_back();
}

void StackPusherHelper::append(const CodeLines &oth) {
	m_code.append(oth);
}

void StackPusherHelper::addTabs(const int qty) {
	m_code.addTabs(qty);
}

void StackPusherHelper::subTabs(const int qty) {
	m_code.subTabs(qty);
}

void StackPusherHelper::pushCont(const CodeLines &cont, const string &comment) {
	if (comment.empty())
		push(0, "PUSHCONT {");
	else
		push(0, "PUSHCONT { ; " + comment);
	for (const auto& l : cont.lines)
		push(0, string("\t") + l);
	push(+1, "}"); // adjust stack // TODO delete +1. For ifelse it's a problem
}

void StackPusherHelper::generateGlobl(const string &fname, const bool isPublic) {
	push(0, ".globl\t" + fname);
	if (isPublic) {
		push(0, ".public\t" + fname);
	}
	push(0, ".type\t"  + fname + ", @function");
}

void StackPusherHelper::generateInternal(const string &fname, const int id) {
	push(0, ".internal-alias :" + fname + ",        " + toString(id));
	push(0, ".internal\t:" + fname);
}

void StackPusherHelper::generateMacro(const string &functionName) {
	push(0, ".macro " + functionName);
}

CodeLines StackPusherHelper::code() const {
	return m_code;
}

const TVMCompilerContext &StackPusherHelper::ctx() const {
	return *m_ctx;
}

void StackPusherHelper::push(int stackDiff, const string &cmd) {
	m_code.push(cmd);
	m_stack.change(stackDiff);
}

void StackPusherHelper::startContinuation() {
	m_code.startContinuation();
}

void StackPusherHelper::endContinuation() {
	m_code.endContinuation();
}

TVMStack &StackPusherHelper::getStack() {
	return m_stack;
}

void StackPusherHelper::pushLines(const std::string &lines) {
	std::istringstream stream{lines};
	std::string line;
	while (std::getline(stream, line)) {
		push(0, line);
	}
}

void StackPusherHelper::untuple(int n) {
	solAssert(0 <= n, "");
	if (n <= 15) {
		push(-1 + n, "UNTUPLE " + toString(n));
	} else {
		solAssert(n <= 255, "");
		pushInt(n);
		push(-2 + n, "UNTUPLEVAR");
	}
}

void StackPusherHelper::index(int index) {
	solAssert(0 <= index, "");
	if (index <= 15) {
		push(-1 + 1, "INDEX " + toString(index));
	} else {
		solAssert(index <= 254, "");
		pushInt(index);
		push(-2 + 1, "INDEXVAR");
	}
}

void StackPusherHelper::set_index(int index) {
	solAssert(0 <= index, "");
	if (index <= 15) {
		push(-2 + 1, "SETINDEX " + toString(index));
	} else {
		solAssert(index <= 254, "");
		pushInt(index);
		push(-1 - 2 + 1, "SETINDEXVAR");
	}
}

void StackPusherHelper::tuple(int qty) {
	solAssert(0 <= qty, "");
	if (qty <= 15) {
		push(-qty + 1, "TUPLE " + toString(qty));
	} else {
		solAssert(qty <= 255, "");
		pushInt(qty);
		push(-1 - qty + 1, "TUPLEVAR");
	}
}

void StackPusherHelper::resetAllStateVars() {
	push(0, ";; set default state vars");
	for (VariableDeclaration const *variable: ctx().notConstantStateVariables()) {
		pushDefaultValue(variable->type());
		setGlob(variable);
	}
	push(0, ";; end set default state vars");
}

void StackPusherHelper::getGlob(VariableDeclaration const *vd) {
	const int index = ctx().getStateVarIndex(vd);
	getGlob(index);
}

void StackPusherHelper::getGlob(int index) {
	solAssert(index >= 0, "");
	if (index <= 31) {
		push(+1, "GETGLOB " + toString(index));
	} else {
		solAssert(index < 255, "");
		pushInt(index);
		push(-1 + 1, "GETGLOBVAR");
	}
}

void StackPusherHelper::setGlob(int index) {
	if (index <= 31) {
		push(-1, "SETGLOB " + toString(index));
	} else {
		solAssert(index < 255, "");
		pushInt(index);
		push(-1 - 1, "SETGLOBVAR");
	}
}

void StackPusherHelper::setGlob(VariableDeclaration const *vd) {
	const int index = ctx().getStateVarIndex(vd);
	solAssert(index >= 0, "");
	setGlob(index);
}

void StackPusherHelper::pushS(int i) {
	solAssert(i >= 0, "");
	if (i == 0) {
		push(+1, "DUP");
	} else {
		push(+1, "PUSH S" + toString(i));
	}
}

void StackPusherHelper::pushInt(int i) {
	push(+1, "PUSHINT " + toString(i));
}

void StackPusherHelper::loadArray(bool directOrder) {
	pushLines(R"(LDU 32
LDDICT
ROTREV
PAIR
)");
	if (directOrder) {
		exchange(0, 1);
	}
	push(-1 + 2, ""); // fix stack
	// stack: array slice
}

void StackPusherHelper::preLoadArray() {
	pushLines(R"(LDU 32
PLDDICT
PAIR
)");
	push(-1 + 1, ""); // fix stack
	// stack: array
}

void StackPusherHelper::load(const Type *type) {
	if (isUsualArray(type)) {
		loadArray();
	} else {
		TypeInfo ti{type};
		solAssert(ti.isNumeric, "");
		string cmd = ti.isSigned ? "LDI " : "LDU ";
		push(-1 + 2, cmd + toString(ti.numBits));
	}
}

void StackPusherHelper::preload(const Type *type) {
	if (isUsualArray(type)) {
		preLoadArray();
	} else if (isIn(type->category(), Type::Category::Mapping, Type::Category::ExtraCurrencyCollection)) {
		push(0, "PLDDICT");
	} else if (type->category() == Type::Category::VarInteger) {
		push(0, "LDVARUINT32");
		push(0, "DROP");
	} else {
		TypeInfo ti{type};
		solAssert(ti.isNumeric, "");
		string cmd = ti.isSigned ? "PLDI " : "PLDU ";
		push(-1 + 1, cmd + toString(ti.numBits));
	}
}

void StackPusherHelper::pushZeroAddress() {
	push(+1, "PUSHSLICE x8000000000000000000000000000000000000000000000000000000000000000001_");
}

void StackPusherHelper::addBinaryNumberToString(std::string &s, u256 value, int bitlen) {
	for (int i = 0; i < bitlen; ++i) {
		s += value % 2 == 0? "0" : "1";
		value /= 2;
	}
	std::reverse(s.rbegin(), s.rbegin() + bitlen);
}

std::string StackPusherHelper::binaryStringToSlice(const std::string &_s) {
	std::string s = _s;
	bool haveCompletionTag = false;
	if (s.size() % 4 != 0) {
		haveCompletionTag = true;
		s += "1";
		s += std::string((4 - s.size() % 4) % 4, '0');
	}
	std::string ans;
	for (int i = 0; i < static_cast<int>(s.length()); i += 4) {
		int x = stoi(s.substr(i, 4), nullptr, 2);
		std::stringstream sstream;
		sstream << std::hex << x;
		ans += sstream.str();
	}
	if (haveCompletionTag) {
		ans += "_";
	}
	return ans;
}

std::string StackPusherHelper::gramsToBinaryString(Literal const *literal) {
	Type const* type = literal->annotation().type;
	u256 value = type->literalValue(literal);
	return gramsToBinaryString(value);
}

std::string StackPusherHelper::gramsToBinaryString(u256 value) {
	std::string s;
	int len = 256;
	for (int i = 0; i < 256; ++i) {
		if (value == 0) {
			len = i;
			break;
		}
		s += value % 2 == 0? "0" : "1";
		value /= 2;
	}
	solAssert(len < 120, "Gram value should fit 120 bit");
	while (len % 8 != 0) {
		s += "0";
		len++;
	}
	std::reverse(s.rbegin(), s.rbegin() + len);
	len = len/8;
	std::string res;
	for (int i = 0; i < 4; ++i) {
		res += len % 2 == 0? "0" : "1";
		len /= 2;
	}
	std::reverse(res.rbegin(), res.rbegin() + 4);
	return res + s;
}

std::string StackPusherHelper::literalToSliceAddress(Literal const *literal, bool pushSlice) {
	Type const* type = literal->annotation().type;
	u256 value = type->literalValue(literal);
//		addr_std$10 anycast:(Maybe Anycast) workchain_id:int8 address:bits256 = MsgAddressInt;
	std::string s;
	s += "10";
	s += "0";
	s += std::string(8, '0');
	addBinaryNumberToString(s, value);
	if (pushSlice)
		push(+1, "PUSHSLICE x" + binaryStringToSlice(s));
	return s;
}

bool StackPusherHelper::tryImplicitConvert(Type const *leftType, Type const *rightType) {
	if (leftType->category() == Type::Category::FixedBytes && rightType->category() == Type::Category::StringLiteral) {
		auto stringLiteralType = to<StringLiteralType>(rightType);
		u256 value = 0;
		for (char c : stringLiteralType->value()) {
			value = value * 256 + c;
		}
		push(+1, "PUSHINT " + toString(value));
		return true;
	}
	return false;
}

void StackPusherHelper::push(const CodeLines &codeLines) {
	for (const std::string& s : codeLines.lines) {
		push(0, s);
	}
}

void StackPusherHelper::pushPrivateFunctionOrMacroCall(const int stackDelta, const string &fname) {
	push(stackDelta, "CALL $" + fname + "$");
}

void StackPusherHelper::pushCall(const string &functionName, const FunctionType *ft) {
	int params  = ft->parameterTypes().size();
	int retVals = ft->returnParameterTypes().size();
	push(-params + retVals, "CALL $" + functionName + "$");
}

void StackPusherHelper::drop(int cnt) {
	solAssert(cnt >= 0, "");
	if (cnt == 0)
		return;

	if (cnt == 1) {
		push(-1, "DROP");
	} else if (cnt == 2) {
		push(-2, "DROP2");
	} else {
		if (cnt > 15) {
			pushInt(cnt);
			push(-(cnt + 1), "DROPX");
		} else {
			push(-cnt, "BLKDROP " + toString(cnt));
		}
	}
}

void StackPusherHelper::blockSwap(int m, int n) {
	solAssert(0 <= m, "");
	solAssert(0 <= n, "");
	if (m == 0 || n == 0) {
		return;
	}
	if (m == 1 && n == 1) {
		exchange(0, 1);
	} else if (m == 1 && n == 2) {
		push(0, "ROT");
	} else if (m == 2 && n == 1) {
		push(0, "ROTREV");
	} else if (m == 2 && n == 2) {
		push(0, "SWAP2");
	} else if (n <= 16 && m <= 16) {
		push(0, "BLKSWAP " + toString(m) + ", " + toString(n));
	} else {
		pushInt(m);
		pushInt(n);
		push(-2, "BLKSWX");
	}
}

void StackPusherHelper::reverse(int i, int j) {
	solAssert(i >= 2, "");
	solAssert(j >= 0, "");
	if (i == 2 && j == 0) {
		push(0, "SWAP");
	} else if (i == 3 && j == 0) {
		push(0, "XCHG s2");
	} else if (i - 2 <= 15 && j <= 15) {
		push(0, "REVERSE " + toString(i) + ", " + toString(j));
	} else {
		pushInt(i);
		pushInt(j);
		push(-2, "REVX");
	}
}

void StackPusherHelper::dropUnder(int leftCount, int droppedCount) {
	// drop dropCount elements that are situated under top leftCount elements
	solAssert(leftCount >= 0, "");
	solAssert(droppedCount >= 0, "");

	auto f = [this, leftCount, droppedCount](){
		if (droppedCount > 15 || leftCount > 15) {
			pushInt(droppedCount);
			pushInt(leftCount);
			push(-2, "BLKSWX");
			drop(droppedCount);
		} else {
			push(-droppedCount, "BLKDROP2 " + toString(droppedCount) + ", " + toString(leftCount));
		}
	};

	if (droppedCount == 0) {
		// nothing do
	} else if (leftCount == 0) {
		drop(droppedCount);
	} else if (droppedCount == 1) {
		if (leftCount == 1) {
			push(-1, "NIP");
		} else {
			f();
		}
	} else if (droppedCount == 2) {
		if (leftCount == 1) {
			push(-1, "NIP");
			push(-1, "NIP");
		} else {
			f();
		}
	} else {
		if (leftCount == 1) {
			exchange(0, droppedCount);
			drop(droppedCount);
		} else {
			f();
		}
	}
}

void StackPusherHelper::exchange(int i, int j) {
	solAssert(i <= j, "");
	solAssert(i >= 0, "");
	solAssert(j >= 1, "");
	if (i == 0 && j <= 255) {
		if (j == 1) {
			push(0, "SWAP");
		} else if (j <= 15) {
			push(0, "XCHG s" + toString(j));
		} else {
			push(0, "XCHG s0,s" + toString(j));
		}
	} else if (i == 1 && 2 <= j && j <= 15) {
		push(0, "XCHG s1,s" + toString(j));
	} else if (1 <= i && i < j && j <= 15) {
		push(0, "XCHG s" + toString(i) + ",s" + toString(j));
	} else if (j <= 255) {
		exchange(0, i);
		exchange(0, j);
		exchange(0, i);
	} else {
		solAssert(false, "");
	}
}

void StackPusherHelper::checkThatKeyCanBeRestored(Type const *keyType, ASTNode const &node) {
	if (isStringOrStringLiteralOrBytes(keyType)) {
		cast_error(node, "Unsupported for mapping key type: " + keyType->toString(true));
	}
}

TypePointer StackPusherHelper::parseIndexType(Type const *type) {
	if (to<ArrayType>(type)) {
		return TypePointer(new IntegerType(32));
	}
	if (auto mappingType = to<MappingType>(type)) {
		return mappingType->keyType();
	}
	if (auto currencyType = to<ExtraCurrencyCollectionType>(type)) {
		return currencyType->keyType();
	}
	solAssert(false, "");
}

TypePointer StackPusherHelper::parseValueType(IndexAccess const &indexAccess) {
	if (auto currencyType = to<ExtraCurrencyCollectionType>(indexAccess.baseExpression().annotation().type)) {
		return currencyType->realValueType();
	}
	return indexAccess.annotation().type;
}

bool StackPusherHelper::tryAssignParam(Declaration const *name) {
	auto& stack = getStack();
	if (stack.isParam(name)) {
		int idx = stack.getOffset(name);
		solAssert(idx >= 0, "");
		if (idx == 0) {
			// nothing
		} else if (idx == 1) {
			push(-1, "NIP");
		} else {
			push(-1, "POP s" + toString(idx));
		}
		return true;
	}
	return false;
}

void StackPusherHelper::ensureValueFitsType(const ElementaryTypeNameToken &typeName, const ASTNode &node) {
	push(0, ";; " + typeName.toString());
	switch (typeName.token()) {
		case Token::IntM:
			push(0, "FITS " + toString(typeName.firstNumber()));
			break;
		case Token::UIntM:
			push(0, "UFITS " + toString(typeName.firstNumber()));
			break;
		case Token::BytesM:
			push(0, "UFITS " + toString(8 * typeName.firstNumber()));
			break;
		case Token::Int:
			push(0, "FITS 256");
			break;
		case Token::Address:
			// Address is a slice
			break;
		case Token::UInt:
			push(0, "UFITS 256");
			break;
		case Token::Bool:
			push(0, "FITS 1");
			break;
		default:
			cast_error(node, "Unimplemented casting");
	}
}

void StackPusherHelper::prepareKeyForDictOperations(Type const *key) {
	// stack: key dict
	if (isStringOrStringLiteralOrBytes(key)) {
		push(+1, "PUSH s1"); // str dict str
		push(-1 + 1, "HASHCU"); // str dict hash
		push(-1, "POP s2"); // hash dict
	}
}

std::pair<std::string, int>
StackPusherHelper::int_msg_info(const std::set<int> &isParamOnStack, const std::map<int, std::string> &constParams) {
	// int_msg_info$0  ihr_disabled:Bool  bounce:Bool(#1)  bounced:Bool
	//                 src:MsgAddress  dest:MsgAddressInt(#4)
	//                 value:CurrencyCollection(#5,#6)  ihr_fee:Grams  fwd_fee:Grams
	//                 created_lt:uint64  created_at:uint32
	//                 = CommonMsgInfoRelaxed;

	// currencies$_ grams:Grams other:ExtraCurrencyCollection = CurrencyCollection;

	const std::vector<int> zeroes {1, 1, 1,
									2, 2,
									4, 1, 4, 4,
									64, 32};
	std::string bitString = "0";
	int maxBitStringSize = 0;
	push(+1, "NEWC");
	for (int param = 0; param < static_cast<int>(zeroes.size()); ++param) {
		solAssert(constParams.count(param) == 0 || isParamOnStack.count(param) == 0, "");

		if (constParams.count(param) != 0) {
			bitString += constParams.at(param);
		} else if (isParamOnStack.count(param) == 0) {
			bitString += std::string(zeroes[param], '0');
			solAssert(param != TvmConst::int_msg_info::dest, "");
		} else {
			appendToBuilder(bitString);
			bitString = "";
			switch (param) {
				case TvmConst::int_msg_info::bounce:
					push(-1, "STI 1");
					++maxBitStringSize;
					break;
				case TvmConst::int_msg_info::dest:
					push(-1, "STSLICE");
					maxBitStringSize += AddressInfo::maxBitLength();
					break;
				case TvmConst::int_msg_info::grams:
					exchange(0, 1);
					push(-1, "STGRAMS");
					maxBitStringSize += 4 + 16 * 8;
					// var_uint$_ {n:#} len:(#< n) value:(uint (len * 8)) = VarUInteger n;
					// nanograms$_ amount:(VarUInteger 16) = Grams;
					break;
				case TvmConst::int_msg_info::currency:
					push(-1, "STDICT");
					break;
				default:
					solAssert(false, "");
			}
		}
	}
	maxBitStringSize += bitString.size();
	return {bitString, maxBitStringSize};
}

std::pair<std::string, int>
StackPusherHelper::ext_msg_info(const set<int> &isParamOnStack) {
	// ext_out_msg_info$11 src:MsgAddressInt dest:MsgAddressExt
	// created_lt:uint64 created_at:uint32 = CommonMsgInfo;

	const std::vector<int> zeroes {2, 2,
								   64, 32};
	std::string bitString = "11";
	int maxBitStringSize = 0;
	push(+1, "NEWC");
	for (int param = 0; param < static_cast<int>(zeroes.size()); ++param) {
		if (isParamOnStack.count(param) == 0) {
			bitString += std::string(zeroes[param], '0');
		} else {
			appendToBuilder(bitString);
			bitString = "";
			if (param == TvmConst::ext_msg_info::dest) {
				push(-1, "STSLICE");
				maxBitStringSize += AddressInfo::maxBitLength();
			} else {
				solAssert(false, "");
			}
		}
	}
	maxBitStringSize += bitString.size();
	return {bitString, maxBitStringSize};
}


void StackPusherHelper::appendToBuilder(const std::string &bitString) {
	// stack: builder
	if (bitString.empty()) {
		return;
	}

	size_t count = std::count_if(bitString.begin(), bitString.end(), [](char c) { return c == '0'; });
	if (count == bitString.size()) {
		stzeroes(count);
	} else {
		const std::string hex = binaryStringToSlice(bitString);
		if (hex.length() * 4 <= 8 * 7 + 1) {
			push(0, "STSLICECONST x" + hex);
		} else {
			push(+1, "PUSHSLICE x" + binaryStringToSlice(bitString));
			push(-1, "STSLICER");
		}
	}
}

void StackPusherHelper::stzeroes(int qty) {
	if (qty > 0) {
		// builder
		if (qty == 1) {
			push(0, "STSLICECONST 0");
		} else {
			pushInt(qty); // builder qty
			push(-1, "STZEROES");
		}
	}
}

void StackPusherHelper::stones(int qty) {
	if (qty > 0) {
		// builder
		if (qty == 1) {
			push(0, "STSLICECONST 1");
		} else {
			pushInt(qty); // builder qty
			push(-1, "STONES");
		}
	}
}

void StackPusherHelper::sendrawmsg() {
	push(-2, "SENDRAWMSG");
}

void StackPusherHelper::sendIntMsg(const std::map<int, Expression const *> &exprs,
								   const std::map<int, std::string> &constParams,
								   const std::function<void(int)> &appendBody,
								   const std::function<void()> &pushSendrawmsgFlag) {
	std::set<int> isParamOnStack;
	for (auto &[param, expr] : exprs | boost::adaptors::reversed) {
		isParamOnStack.insert(param);
		TVMExpressionCompiler{*this}.compileNewExpr(expr);
	}
	sendMsg(isParamOnStack, constParams, appendBody, nullptr, pushSendrawmsgFlag, true);
}

void StackPusherHelper::sendMsg(const std::set<int>& isParamOnStack,
								const std::map<int, std::string> &constParams,
								const std::function<void(int)> &appendBody,
								const std::function<void()> &appendStateInit,
								const std::function<void()> &pushSendrawmsgFlag,
								bool isInternalMessage) {
	std::string bitString;
	int msgInfoSize;
	if (isInternalMessage) {
		std::tie(bitString, msgInfoSize) = int_msg_info(isParamOnStack, constParams);
	} else {
		std::tie(bitString, msgInfoSize) = ext_msg_info(isParamOnStack);
	}
	// stack: builder
	appendToBuilder(bitString);

	if (appendStateInit) {
		// stack: values... builder
		appendToBuilder("1");
		appendStateInit();
		++msgInfoSize;
		// stack: builder-with-stateInit
	} else {
		appendToBuilder("0"); // there is no StateInit
	}

	++msgInfoSize;

	if (appendBody) {
		// stack: values... builder
		appendBody(msgInfoSize);
		// stack: builder-with-body
	} else {
		appendToBuilder("0"); // there is no body
	}

	// stack: builder'
	push(0, "ENDC"); // stack: cell
	if (pushSendrawmsgFlag) {
		pushSendrawmsgFlag();
	} else {
		pushInt(TvmConst::SENDRAWMSG::DefaultFlag);
	}
	sendrawmsg();
}

CodeLines solidity::frontend::switchSelectorIfNeed(FunctionDefinition const *f) {
	FunctionUsageScanner scanner{*f};
	CodeLines code;
	if (scanner.havePrivateFunctionCall) {
		code.push("PUSHINT 1");
		code.push("CALL 1");
	}
	return code;
}

int TVMStack::size() const {
	return m_size;
}

void TVMStack::change(int diff) {
	m_size += diff;
	solAssert(m_size >= 0, "");
}

bool TVMStack::isParam(Declaration const *name) const {
	return m_params.count(name) > 0;
}

void TVMStack::add(Declaration const *name, bool doAllocation) {
	solAssert(m_params.count(name) == 0, "");
	m_params[name] = doAllocation? m_size++ : m_size - 1;
}

int TVMStack::getOffset(Declaration const *name) const {
	solAssert(isParam(name), "");
	return getOffset(m_params.at(name));
}

int TVMStack::getOffset(int stackPos) const {
	return m_size - 1 - stackPos;
}

int TVMStack::getStackSize(Declaration const *name) const {
	return m_params.at(name);
}

void TVMStack::ensureSize(int savedStackSize, const string &location) const {
	solAssert(savedStackSize == m_size, "stack: " + toString(savedStackSize)
										+ " vs " + toString(m_size) + " at " + location);
}

string CodeLines::str(const string &indent) const {
	std::ostringstream o;
	for (const string& s : lines) {
		o << indent << s << endl;
	}
	return o.str();
}

void CodeLines::addTabs(const int qty) {
	tabQty += qty;
}

void CodeLines::subTabs(const int qty) {
	tabQty -= qty;
}

void CodeLines::startContinuation() {
	push("PUSHCONT {");
	++tabQty;
}

void CodeLines::endContinuation() {
	--tabQty;
	push("}");
	solAssert(tabQty >= 0, "");
}

void CodeLines::push(const string &cmd) {
	if (cmd.empty() || cmd == "\n") {
		return;
	}

	// space means empty line
	if (cmd == " ")
		lines.emplace_back("");
	else {
		solAssert(tabQty >= 0, "");
		lines.push_back(std::string(tabQty, '\t') + cmd);
	}
}

void CodeLines::append(const CodeLines &oth) {
	for (const auto& s : oth.lines) {
		lines.push_back(std::string(tabQty, '\t') + s);
	}
}

void TVMCompilerContext::addFunction(FunctionDefinition const *_function) {
	if (!_function->isConstructor()) {
		string name = functionName(_function);
		m_functions[name] = _function;
	}
}

void TVMCompilerContext::initMembers(ContractDefinition const *contract) {
	solAssert(!m_contract, "");
	m_contract = contract;
	for (const auto &pair : getContractFunctionPairs(contract)) {
		m_function2contract.insert(pair);
	}

	for (ContractDefinition const* base : contract->annotation().linearizedBaseContracts) {
		for (FunctionDefinition const* f : base->definedFunctions()) {
			ignoreIntOverflow |= f->name() == "tvm_ignore_integer_overflow";
			if (f->name() == "offchainConstructor") {
				if (m_haveOffChainConstructor) {
					cast_error(*f, "This function can not be overrived/overloaded.");
				} else {
					m_haveOffChainConstructor = true;
				}
			}
			haveFallback |= f->isFallback();
			haveOnBounce |= f->isOnBounce();
			haveReceive |= f->isReceive();
		}
	}

	ignoreIntOverflow |= m_pragmaHelper.haveIgnoreIntOverflow();
	for (const auto f : getContractFunctions(contract)) {
		if (isPureFunction(f))
			continue;
		addFunction(f);
	}
	for (VariableDeclaration const *variable: notConstantStateVariables()) {
		m_stateVarIndex[variable] = 10 + m_stateVarIndex.size();
	}
}

TVMCompilerContext::TVMCompilerContext(ContractDefinition const *contract,
									   PragmaDirectiveHelper const &pragmaHelper) : m_pragmaHelper{pragmaHelper} {
	initMembers(contract);
}

int TVMCompilerContext::getStateVarIndex(VariableDeclaration const *variable) const {
	return m_stateVarIndex.at(variable);
}

std::vector<VariableDeclaration const *> TVMCompilerContext::notConstantStateVariables() const {
	std::vector<VariableDeclaration const*> variableDeclarations;
	std::vector<ContractDefinition const*> mainChain = getContractsChain(getContract());
	for (ContractDefinition const* contract : mainChain) {
		for (VariableDeclaration const *variable: contract->stateVariables()) {
			if (!variable->isConstant()) {
				variableDeclarations.push_back(variable);
			}
		}
	}
	return variableDeclarations;
}

PragmaDirectiveHelper const &TVMCompilerContext::pragmaHelper() const {
	return m_pragmaHelper;
}

bool TVMCompilerContext::haveTimeInAbiHeader() const {
	if (m_pragmaHelper.abiVersion() == 1) {
		return true;
	}
	if (m_pragmaHelper.abiVersion() == 2) {
		return m_pragmaHelper.haveTime() || afterSignatureCheck() == nullptr;
	}
	solAssert(false, "");
}

bool TVMCompilerContext::isStdlib() const {
	return m_contract->name() == "stdlib";
}

string TVMCompilerContext::getFunctionInternalName(FunctionDefinition const *_function) const {
	if (isStdlib()) {
		return _function->name();
	}
	if (_function->name() == "onCodeUpgrade") {
		return ":onCodeUpgrade";
	}
	return _function->name() + "_internal";
}

string TVMCompilerContext::getFunctionExternalName(FunctionDefinition const *_function) {
	const string& fname = _function->name();
	solAssert(_function->isPublic(), "Internal error: expected public function: " + fname);
	if (_function->isConstructor()) {
		return "constructor";
	}
	if (_function->isFallback()) {
		return "fallback";
	}
	return fname;
}

bool TVMCompilerContext::isPureFunction(FunctionDefinition const *f) const {
	const auto& vec = getContract(f)->annotation().unimplementedFunctions;
	return std::find(vec.cbegin(), vec.cend(), f) != vec.end();
}

const ContractDefinition *TVMCompilerContext::getContract() const {
	return m_contract;
}

const ContractDefinition *TVMCompilerContext::getContract(const FunctionDefinition *f) const {
	return m_function2contract.at(f);
}

const FunctionDefinition *TVMCompilerContext::getLocalFunction(const string& fname) const {
	return get_from_map(m_functions, fname, nullptr);
}

bool TVMCompilerContext::haveFallbackFunction() const {
	return haveFallback;
}

bool TVMCompilerContext::haveReceiveFunction() const {
	return haveReceive;
}

bool TVMCompilerContext::haveOnBounceHandler() const {
	return haveOnBounce;
}

bool TVMCompilerContext::ignoreIntegerOverflow() const {
	return ignoreIntOverflow;
}

bool TVMCompilerContext::haveOffChainConstructor() const {
	return m_haveOffChainConstructor;
}

FunctionDefinition const *TVMCompilerContext::afterSignatureCheck() const {
	for (FunctionDefinition const* f : m_contract->definedFunctions()) {
		if (f->name() == "afterSignatureCheck") {
			return f;
		}
	}
	return nullptr;
}

bool TVMCompilerContext::storeTimestampInC4() const {
	return haveTimeInAbiHeader() && afterSignatureCheck() == nullptr;
}

void StackPusherHelper::pushDefaultValue(Type const* type, bool isResultBuilder) {
	Type::Category cat = type->category();
	switch (cat) {
		case Type::Category::Address:
		case Type::Category::Contract:
			pushZeroAddress();
			if (isResultBuilder) {
				push(+1, "NEWC");
				push(-1, "STSLICE");
			}
			break;
		case Type::Category::Bool:
		case Type::Category::FixedBytes:
		case Type::Category::Integer:
		case Type::Category::Enum:
		case Type::Category::VarInteger:
			push(+1, "PUSHINT 0");
			if (isResultBuilder) {
				push(+1, "NEWC");
				push(-1, storeIntegralOrAddress(type, false));
			}
			break;
		case Type::Category::Array:
			if (to<ArrayType>(type)->isByteArray()) {
				push(+1, "NEWC");
				if (!isResultBuilder) {
					push(0, "ENDC");
				}
				break;
			}
			if (!isResultBuilder) {
				pushInt(0);
				push(+1, "NEWDICT");
				push(-2 + 1, "PAIR");
			} else {
				push(+1, "NEWC");
				pushInt(33);
				push(-1, "STZEROES");
			}
			break;
		case Type::Category::Mapping:
		case Type::Category::ExtraCurrencyCollection:
			if (isResultBuilder) {
				push(+1, "NEWC");
				stzeroes(1);
			} else {
				push(+1, "NEWDICT");
			}
			break;
		case Type::Category::Struct: {
			auto structType = to<StructType>(type);
			StructCompiler structCompiler{this, structType};
			structCompiler.createDefaultStruct(isResultBuilder);
			break;
		}
		case Type::Category::TvmSlice:
			push(+1, "PUSHSLICE x8_");
			if (isResultBuilder) {
				push(+1, "NEWC");
				push(-1, "STSLICE");
			}
			break;
		case Type::Category::TvmBuilder:
			push(+1, "NEWC");
			break;
		case Type::Category::TvmCell:
			push(+1, "NEWC");
			if (!isResultBuilder) {
				push(0, "ENDC");
			}
			break;
		case Type::Category::Function: {
			solAssert(!isResultBuilder, "");
			auto functionType = to<FunctionType>(type);
			StackPusherHelper pusherHelper(&ctx(), functionType->parameterTypes().size());
			pusherHelper.drop(functionType->parameterTypes().size());
			for (const TypePointer &param : functionType->returnParameterTypes()) {
				pusherHelper.pushDefaultValue(param);
			}
			pushCont(pusherHelper.code());
			break;
		}
		default:
			solAssert(false, "");
	}
}

class GetFromDict : public DictOperation {
public:
	GetFromDict(StackPusherHelper& pusher, Type const& keyType, Type const& valueType, ASTNode const& node,
				const StackPusherHelper::GetDictOperation op,
                const bool resultAsSliceForStruct) :
		DictOperation{pusher, keyType, valueType, node},
		haveValue{&pusher.ctx()}, // for Fetch
		op{op},
		resultAsSliceForStruct{resultAsSliceForStruct} {

	}

	void getDict() {
		// if op == GetSetFromMapping than stack: value key dict
		// else                            stack: key dict
		pusher.prepareKeyForDictOperations(&keyType);

		pusher.pushInt(keyLength); // push int on stack
		const int stackDelta = isIn(op, StackPusherHelper::GetDictOperation::GetSetFromMapping,
		                                StackPusherHelper::GetDictOperation::GetAddFromMapping,
		                                StackPusherHelper::GetDictOperation::GetReplaceFromMapping)? -4 + 3 : -3 + 2;

		haveValue.push(0, "SWAP");

		std::string opcode = "DICT" + typeToDictChar(&keyType);
		switch (op) {
			case StackPusherHelper::GetDictOperation::GetSetFromMapping:
			case StackPusherHelper::GetDictOperation::GetAddFromMapping:
			case StackPusherHelper::GetDictOperation::GetReplaceFromMapping:
				if (op == StackPusherHelper::GetDictOperation::GetSetFromMapping)
					opcode += "SETGET";
				else if (op == StackPusherHelper::GetDictOperation::GetAddFromMapping)
					opcode += "ADDGET";
				else if (op == StackPusherHelper::GetDictOperation::GetReplaceFromMapping)
					opcode += "REPLACEGET";
				else
					solAssert(false, "");

				if (isIn(valueCategory, Type::Category::Address, Type::Category::Contract) || isByteArrayOrString(&valueType)){ // TOOO to var and use that
					// do nothing
				} else if (valueCategory == Type::Category::TvmCell ||
				           (valueCategory == Type::Category::Struct && !StructCompiler::isCompatibleWithSDK(keyLength, to<StructType>(&valueType)))) {
					opcode += "REF";
				} else {
					opcode += "B";
				}
				break;
			case StackPusherHelper::GetDictOperation::Exist:
			case StackPusherHelper::GetDictOperation::Fetch:
			case StackPusherHelper::GetDictOperation::GetFromArray:
			case StackPusherHelper::GetDictOperation::GetFromMapping:
				opcode += "GET";
				if (isIn(valueCategory, Type::Category::TvmCell) ||
				    (valueCategory == Type::Category::Struct && !StructCompiler::isCompatibleWithSDK(keyLength, to<StructType>(&valueType))) ||
				    isByteArrayOrString(&valueType)) {
					opcode += "REF";
				}
				break;
		}

		pusher.push(stackDelta, opcode);

		doDictOperation();
	}

protected:
	void onCell() override {
		switch (op) {
			case StackPusherHelper::GetDictOperation::GetFromMapping:
				pushContinuationWithDefaultValue();
				pusher.push(-2, "IFNOT");
				break;
			case StackPusherHelper::GetDictOperation::GetSetFromMapping:
			case StackPusherHelper::GetDictOperation::GetReplaceFromMapping:
				pusher.pushS(0);
				pushContinuationWithDefaultValue(StatusFlag::Non, true);
				pusher.push(-2, "IFNOT");
				break;
			case StackPusherHelper::GetDictOperation::GetAddFromMapping:
				pusher.pushS(0);
				pushContinuationWithDefaultValue(StatusFlag::Non, true);
				pusher.push(-2, "IF");
				break;
			case StackPusherHelper::GetDictOperation::GetFromArray:
				pusher.push(-1, "THROWIFNOT " + toString(TvmConst::RuntimeException::ArrayIndexOutOfRange));
				break;
			case StackPusherHelper::GetDictOperation::Fetch:
				fetchValue();
				break;
			case StackPusherHelper::GetDictOperation::Exist:
				checkExist();
				break;
		}
	}

	void onSmallStruct() override {
		StructCompiler sc{&pusher, to<StructType>(&valueType)};
		switch (op) {
			case StackPusherHelper::GetDictOperation::GetFromMapping:
				if (resultAsSliceForStruct) {
					pushContinuationWithDefaultValue();
					pusher.push(-2, "IFNOT");
				} else {
					// ok
					pusher.startContinuation();
					sc.convertSliceToTuple();
					pusher.endContinuation();
					// fail
					pusher.startContinuation();
					sc.createDefaultStruct(false);
					pusher.endContinuation();
					pusher.push(-2, "IFELSE");
				}
				break;
			case StackPusherHelper::GetDictOperation::GetSetFromMapping:
			case StackPusherHelper::GetDictOperation::GetReplaceFromMapping: {
				solAssert(!resultAsSliceForStruct, "");
				// ok
				pusher.startContinuation();
				sc.convertSliceToTuple();
				pusher.push(0, "TRUE");
				pusher.endContinuation();
				// fail
				pushContinuationWithDefaultValue(StatusFlag::False);
				//
				pusher.push(-1, "IFELSE");
				break;
			}
			case StackPusherHelper::GetDictOperation::GetAddFromMapping: {
				solAssert(!resultAsSliceForStruct, "");
				// ok
				pushContinuationWithDefaultValue(StatusFlag::True);
				// fail
				pusher.startContinuation();
				sc.convertSliceToTuple();
				pusher.push(0, "FALSE");
				pusher.endContinuation();
				//
				pusher.push(-1, "IFELSE");
				break;
			}
			case StackPusherHelper::GetDictOperation::GetFromArray:
				pusher.push(-1, "THROWIFNOT " + toString(TvmConst::RuntimeException::ArrayIndexOutOfRange));
				if (!resultAsSliceForStruct) {
					sc.convertSliceToTuple();
				}
				break;
			case StackPusherHelper::GetDictOperation::Fetch: {
				StructCompiler{&haveValue, to<StructType>(&valueType)}.convertSliceToTuple();
				fetchValue();
				break;
			}
			case StackPusherHelper::GetDictOperation::Exist:
				checkExist();
				break;
		}
	}
	void onLargeStruct() override {
		StructCompiler sc{&pusher, to<StructType>(&valueType)};
		switch (op) {
			case StackPusherHelper::GetDictOperation::GetFromMapping: {
				StackPusherHelper pusherHelper(&pusher.ctx());
				pusherHelper.push(-1 + 1, "CTOS");
				if (!resultAsSliceForStruct) {
					StructCompiler{&pusherHelper, to<StructType>(&valueType)}.convertSliceToTuple();
				}
				pusher.pushCont(pusherHelper.code());
				pushContinuationWithDefaultValue();
				pusher.push(-3, "IFELSE");
				break;
			}
			case StackPusherHelper::GetDictOperation::GetSetFromMapping:
			case StackPusherHelper::GetDictOperation::GetReplaceFromMapping:
				solAssert(!resultAsSliceForStruct, "");
				// ok
				pusher.startContinuation();
				pusher.push(0, "CTOS");
				sc.convertSliceToTuple();
				pusher.push(0, "TRUE");
				pusher.endContinuation();
				// fail
				pushContinuationWithDefaultValue(StatusFlag::False);
				pusher.push(-1, ""); // fix stack
				//
				pusher.push(0, "IFELSE");
				break;
			case StackPusherHelper::GetDictOperation::GetAddFromMapping:
				solAssert(!resultAsSliceForStruct, "");
				// ok
				pushContinuationWithDefaultValue(StatusFlag::True);
				// fail
				pusher.startContinuation();
				pusher.push(0, "CTOS");
				sc.convertSliceToTuple();
				pusher.push(0, "FALSE");
				pusher.endContinuation();
				pusher.push(-1, "IFELSE");
				break;
			case StackPusherHelper::GetDictOperation::GetFromArray:
				pusher.push(-1, "THROWIFNOT " + toString(TvmConst::RuntimeException::ArrayIndexOutOfRange));
				pusher.push(-1 + 1, "CTOS");
				if (!resultAsSliceForStruct) {
					sc.convertSliceToTuple();
				}
				break;
			case StackPusherHelper::GetDictOperation::Fetch: {
				haveValue.push(0, "CTOS");
				StructCompiler{&haveValue, to<StructType>(&valueType)}.convertSliceToTuple();
				fetchValue();
				break;
			}
			case StackPusherHelper::GetDictOperation::Exist:
				checkExist();
				break;
		}
	}

	void onAddress() override {
		onByteArrayOrString();
	}

	void onByteArrayOrString() override {
		switch (op) {
			case StackPusherHelper::GetDictOperation::GetFromMapping:
				pushContinuationWithDefaultValue();
				pusher.push(-2, "IFNOT");
				break;
			case StackPusherHelper::GetDictOperation::GetSetFromMapping:
			case StackPusherHelper::GetDictOperation::GetReplaceFromMapping:
				pusher.pushS(0);
				pushContinuationWithDefaultValue(StatusFlag::Non, true);
				pusher.push(-2, "IFNOT");
				break;
			case StackPusherHelper::GetDictOperation::GetAddFromMapping:
				pusher.pushS(0);
				pushContinuationWithDefaultValue(StatusFlag::Non, true);
				pusher.push(-2, "IF");
				break;
			case StackPusherHelper::GetDictOperation::GetFromArray:
				pusher.push(-1, "THROWIFNOT " + toString(TvmConst::RuntimeException::ArrayIndexOutOfRange));
				break;
			case StackPusherHelper::GetDictOperation::Fetch: {
				fetchValue();
				break;
			}
			case StackPusherHelper::GetDictOperation::Exist:
				checkExist();
				break;
		}
	}

	void onIntegralOrArrayOrVarInt() override {
		switch (op) {
			case StackPusherHelper::GetDictOperation::GetFromMapping: {
				StackPusherHelper pusherHelper(&pusher.ctx());

				pusherHelper.preload(&valueType);
				pusher.pushCont(pusherHelper.code());

				pushContinuationWithDefaultValue();
				pusher.push(-3, "IFELSE");
				break;
			}
			case StackPusherHelper::GetDictOperation::GetSetFromMapping:
			case StackPusherHelper::GetDictOperation::GetReplaceFromMapping:
				// ok
				pusher.startContinuation();
				pusher.preload(&valueType);
				pusher.push(0, "TRUE");
				pusher.endContinuation();
				// fail
				pushContinuationWithDefaultValue(StatusFlag::False);
				pusher.push(-1, ""); // fix stack
				//
				pusher.push(0, "IFELSE");
				break;
			case StackPusherHelper::GetDictOperation::GetAddFromMapping:
				// ok
				pushContinuationWithDefaultValue(StatusFlag::True);
				// fail
				pusher.startContinuation();
				pusher.preload(&valueType);
				pusher.push(0, "FALSE");
				pusher.endContinuation();
				//
				pusher.push(-1, "IFELSE");
				break;
			case StackPusherHelper::GetDictOperation::GetFromArray:
				pusher.push(-1, "THROWIFNOT " + toString(TvmConst::RuntimeException::ArrayIndexOutOfRange));
				pusher.preload(&valueType);
				break;
			case StackPusherHelper::GetDictOperation::Fetch: {
				haveValue.preload(&valueType);
				fetchValue();
				break;
			}
			case StackPusherHelper::GetDictOperation::Exist:
				checkExist();
				break;
		}
	}

	void onMapOrECC() override {
		onIntegralOrArrayOrVarInt();
	}

private:
	enum class StatusFlag { True, False, Non };

	void pushContinuationWithDefaultValue(StatusFlag flag = StatusFlag::Non, bool doSwap = false) {
		StackPusherHelper pusherHelper(&pusher.ctx());
		if (valueCategory == Type::Category::Struct) {
			if (resultAsSliceForStruct) {
				pusherHelper.pushDefaultValue(&valueType, true);
				pusherHelper.push(0, "ENDC");
				pusherHelper.push(0, "CTOS");
			} else {
				pusherHelper.pushDefaultValue(&valueType, false);
			}
		} else {
			pusherHelper.pushDefaultValue(&valueType);
		}

		switch (flag) {
			case StatusFlag::True:
				pusherHelper.push(+1, "TRUE");
				break;
			case StatusFlag::False:
				pusherHelper.push(+1, "FALSE");
				break;
			case StatusFlag::Non:
				break;
		}
		if (doSwap) {
			pusherHelper.exchange(0, 1);
		}
		pusher.pushCont(pusherHelper.code());
	}

	void fetchValue() {
		StackPusherHelper noValue(&pusher.ctx());
		if (valueCategory == Type::Category::Struct) {
			noValue.push(0, "NULL"); // TODO use NULLSWAPIFNOT
		} else {
			noValue.pushDefaultValue(&valueType, false);
		}

		pusher.push(0, "DUP");
		pusher.pushCont(haveValue.code());
		pusher.pushCont(noValue.code());
		pusher.push(-2, "IFELSE");
	}

	void checkExist() {
		StackPusherHelper nip(&pusher.ctx());
		nip.push(+1, ""); // fix stack
		nip.push(-1, "NIP"); // delete value

		pusher.push(0, "DUP");
		pusher.pushCont(nip.code());
		pusher.push(-2, "IF");
	}

protected:
	StackPusherHelper haveValue;
	const StackPusherHelper::GetDictOperation op;
	const bool resultAsSliceForStruct;
};

void StackPusherHelper::getDict(Type const& keyType, Type const& valueType, ASTNode const& node,
                                const GetDictOperation op,
                                const bool resultAsSliceForStruct) {

	GetFromDict d(*this, keyType, valueType, node, op, resultAsSliceForStruct);
	d.getDict();
}


