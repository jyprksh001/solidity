/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <tools/yulPhaser/Exceptions.h>
#include <tools/yulPhaser/Program.h>

#include <libyul/optimiser/BlockFlattener.h>
#include <libyul/optimiser/Metrics.h>
#include <libyul/optimiser/StructuralSimplifier.h>
#include <libyul/AsmData.h>

#include <libsolutil/CommonIO.h>
#include <libsolutil/JSON.h>
#include <liblangutil/CharStream.h>

#include <boost/test/unit_test.hpp>

#include <cassert>
#include <regex>
#include <string>

using namespace std;
using namespace solidity::langutil;
using namespace solidity::util;
using namespace solidity::yul;
using namespace boost::unit_test::framework;

namespace
{
	/// If the specified block is redundant (i.e. the only thing it contains is another block)
	/// the function recurses into it and returns the first non-redundant one it finds.
	/// If the block isn't redundant it just returns it immediately.
	Block const& skipRedundantBlocks(Block const& _block)
	{
		if (_block.statements.size() == 1 && holds_alternative<Block>(_block.statements[0]))
			return skipRedundantBlocks(get<Block>(_block.statements[0]));
		else
			return _block;
	}

	string stripWhitespace(string const& input)
	{
		regex whitespaceRegex("\\s+");
		return regex_replace(input, whitespaceRegex, "");
	}
}

namespace solidity::phaser::test
{

BOOST_AUTO_TEST_SUITE(Phaser)
BOOST_AUTO_TEST_SUITE(ProgramTest)

BOOST_AUTO_TEST_CASE(load_should_rewind_the_stream)
{
	string sourceCode(
		"{\n"
		"    let x := 1\n"
		"    let y := 2\n"
		"}\n"
	);
	CharStream sourceStream(sourceCode, current_test_case().p_name);
	sourceStream.setPosition(5);

	auto program = Program::load(sourceStream);

	BOOST_TEST(CodeSize::codeSize(program.ast()) == 2);
}

BOOST_AUTO_TEST_CASE(load_should_disambiguate)
{
	string sourceCode(
		"{\n"
		"    {\n"
		"        let x := 1\n"
		"    }\n"
		"    {\n"
		"        let x := 2\n"
		"    }\n"
		"}\n"
	);
	CharStream sourceStream(sourceCode, current_test_case().p_name);
	auto program = Program::load(sourceStream);

	// skipRedundantBlocks() makes the test independent of whether load() includes function grouping or not.
	Block const& parentBlock = skipRedundantBlocks(program.ast());
	BOOST_TEST(parentBlock.statements.size() == 2);

	Block const& innerBlock1 = get<Block>(parentBlock.statements[0]);
	Block const& innerBlock2 = get<Block>(parentBlock.statements[1]);
	VariableDeclaration const& declaration1 = get<VariableDeclaration>(innerBlock1.statements[0]);
	VariableDeclaration const& declaration2 = get<VariableDeclaration>(innerBlock2.statements[0]);

	BOOST_TEST(declaration1.variables[0].name.str() == "x");
	BOOST_TEST(declaration2.variables[0].name.str() != "x");
}

BOOST_AUTO_TEST_CASE(load_should_do_function_grouping_and_hoisting)
{
	string sourceCode(
		"{\n"
		"    function foo() -> result\n"
		"    {\n"
		"        result := 1\n"
		"    }\n"
		"    let x := 1\n"
		"    function bar(a) -> result\n"
		"    {\n"
		"        result := 2\n"
		"    }\n"
		"    let y := 2\n"
		"}\n"
	);
	CharStream sourceStream(sourceCode, current_test_case().p_name);
	auto program = Program::load(sourceStream);

	BOOST_TEST(program.ast().statements.size() == 3);
	BOOST_TEST(holds_alternative<Block>(program.ast().statements[0]));
	BOOST_TEST(holds_alternative<FunctionDefinition>(program.ast().statements[1]));
	BOOST_TEST(holds_alternative<FunctionDefinition>(program.ast().statements[2]));
}

BOOST_AUTO_TEST_CASE(load_should_do_loop_init_rewriting)
{
	string sourceCode(
		"{\n"
		"    for { let i := 0 } true {}\n"
		"    {\n"
		"    }\n"
		"}\n"
	);
	CharStream sourceStream(sourceCode, current_test_case().p_name);
	auto program = Program::load(sourceStream);

	// skipRedundantBlocks() makes the test independent of whether load() includes function grouping or not.
	Block const& parentBlock = skipRedundantBlocks(program.ast());
	BOOST_TEST(holds_alternative<VariableDeclaration>(parentBlock.statements[0]));
	BOOST_TEST(holds_alternative<ForLoop>(parentBlock.statements[1]));
}

BOOST_AUTO_TEST_CASE(load_should_throw_InvalidProgram_if_program_cant_be_parsed)
{
	string sourceCode("invalid program\n");
	CharStream sourceStream(sourceCode, current_test_case().p_name);

	BOOST_CHECK_THROW(Program::load(sourceStream), InvalidProgram);
}

BOOST_AUTO_TEST_CASE(load_should_throw_InvalidProgram_if_program_cant_be_analyzed)
{
	// This should be parsed just fine but fail the analysis with:
	//     Error: Variable not found or variable not lvalue.
	string sourceCode(
		"{\n"
		"    x := 1\n"
		"}\n"
	);
	CharStream sourceStream(sourceCode, current_test_case().p_name);

	BOOST_CHECK_THROW(Program::load(sourceStream), InvalidProgram);
}

BOOST_AUTO_TEST_CASE(optimise)
{
	string sourceCode(
		"{\n"
		"    {\n"
		"        if 1 { let x := 1 }\n"
		"        if 0 { let y := 2 }\n"
		"    }\n"
		"}\n"
	);
	CharStream sourceStream(sourceCode, current_test_case().p_name);
	auto program = Program::load(sourceStream);

	[[maybe_unused]] Block const& parentBlockBefore = skipRedundantBlocks(program.ast());
	assert(parentBlockBefore.statements.size() == 2);
	assert(holds_alternative<If>(parentBlockBefore.statements[0]));
	assert(holds_alternative<If>(parentBlockBefore.statements[1]));

	program.optimise({StructuralSimplifier::name, BlockFlattener::name});

	Block const& parentBlockAfter = program.ast();
	BOOST_TEST(parentBlockAfter.statements.size() == 1);
	BOOST_TEST(holds_alternative<VariableDeclaration>(parentBlockAfter.statements[0]));
}

BOOST_AUTO_TEST_CASE(output_operator)
{
	string sourceCode(
		"{\n"
		"    let factor := 13\n"
		"    {\n"
		"        if factor\n"
		"        {\n"
		"            let variable := add(1, 2)\n"
		"        }\n"
		"        let result := factor\n"
		"    }\n"
		"    let something := 6\n"
		"    let something_else := mul(something, factor)\n"
		"}\n"
	);
	CharStream sourceStream(sourceCode, current_test_case().p_name);
	auto program = Program::load(sourceStream);

	// NOTE: The snippet above was chosen so that the few optimisations applied automatically by load()
	// as of now do not change the code significantly. If that changes, you may have to update it.
	BOOST_TEST(stripWhitespace(toString(program)) == stripWhitespace("{" + sourceCode + "}"));
}

BOOST_AUTO_TEST_CASE(toJson)
{
	string sourceCode(
		"{\n"
		"    let a := 3\n"
		"    if a\n"
		"    {\n"
		"        let abc := add(1, 2)\n"
		"    }\n"
		"}\n"
	);
	CharStream sourceStream(sourceCode, current_test_case().p_name);
	auto program = Program::load(sourceStream);

	Json::Value parsingResult;
	string errors;
	BOOST_TEST(jsonParseStrict(program.toJson(), parsingResult, &errors));
	BOOST_TEST(errors.empty());
}

BOOST_AUTO_TEST_CASE(codeSize)
{
	string sourceCode(
		"{\n"
		"    function foo() -> result\n"
		"    {\n"
		"        result := 15\n"
		"    }\n"
		"    let a := 1\n"
		"}\n"
	);
	CharStream sourceStream(sourceCode, current_test_case().p_name);
	auto program = Program::load(sourceStream);

	BOOST_TEST(program.codeSize() == CodeSize::codeSizeIncludingFunctions(program.ast()));
}

BOOST_AUTO_TEST_SUITE_END()
BOOST_AUTO_TEST_SUITE_END()

}
