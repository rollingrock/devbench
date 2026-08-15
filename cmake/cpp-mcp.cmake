# In-tree build of cpp-mcp (https://github.com/hkr04/cpp-mcp), vendored as the
# lib/cpp-mcp submodule. The CMake twin of xmake/cpp-mcp.lua — same TU selection,
# same two source edits, same defines — so the Fallout and Skyrim builds link the
# same MCP behaviour and a protocol change lands in one place.
#
# Only the SERVER-side TUs are compiled; the stdio/SSE *client* implementations are
# intentionally omitted (devbench is a server only).
#
# Two edits are applied into a build-tree header mirror so the submodule stays clean:
#   1. mcp_message.h: `#include "json.hpp"` -> `#include <nlohmann/json.hpp>`, so
#      cpp-mcp and devbench share ONE nlohmann_json ABI. The vendored 3.11.3 and our
#      3.12.0 wrap their API in different ABI-tagged inline namespaces; mixing them
#      is an LNK2001 at link time, not a compile error, so this is not optional.
#   2. mcp_server.h: add a public `http()` getter returning the underlying
#      httplib::Server*, so the REST facade can mount routes on the MCP port.
#
# Both edits VERIFY their anchor and hard-fail if upstream moved it. A silently
# skipped patch here surfaces as a link error or a missing REST facade much later.

set(CPP_MCP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/lib/cpp-mcp")
set(CPP_MCP_PATCHED_INC "${CMAKE_BINARY_DIR}/cpp-mcp-patched/include")

if(NOT EXISTS "${CPP_MCP_ROOT}/src/mcp_server.cpp")
	message(FATAL_ERROR
		"cpp-mcp submodule missing. Run: git submodule update --init --recursive lib/cpp-mcp")
endif()

file(MAKE_DIRECTORY "${CPP_MCP_PATCHED_INC}")
file(GLOB _cpp_mcp_headers "${CPP_MCP_ROOT}/include/*.h")
foreach(_hdr ${_cpp_mcp_headers})
	get_filename_component(_name "${_hdr}" NAME)
	file(READ "${_hdr}" _content)

	if(_name STREQUAL "mcp_message.h")
		string(FIND "${_content}" "#include \"json.hpp\"" _found)
		if(_found EQUAL -1)
			message(FATAL_ERROR
				"cpp-mcp: expected `#include \"json.hpp\"` in mcp_message.h; upstream changed — review cmake/cpp-mcp.cmake")
		endif()
		string(REPLACE "#include \"json.hpp\"" "#include <nlohmann/json.hpp>" _content "${_content}")

	elseif(_name STREQUAL "mcp_server.h")
		set(_anchor "\nprivate:\n    std::string host_;")
		string(FIND "${_content}" "${_anchor}" _found)
		if(_found EQUAL -1)
			message(FATAL_ERROR
				"cpp-mcp: expected the private-section anchor in mcp_server.h; upstream changed — review cmake/cpp-mcp.cmake")
		endif()
		set(_getter "\n    /**\n     * @brief Access the underlying httplib server to register custom routes.\n     */\n    httplib::Server* http() { return http_server_.get(); }\n${_anchor}")
		string(REPLACE "${_anchor}" "${_getter}" _content "${_content}")
	endif()

	file(WRITE "${CPP_MCP_PATCHED_INC}/${_name}" "${_content}")
endforeach()

add_library(cpp-mcp STATIC
	"${CPP_MCP_ROOT}/src/mcp_message.cpp"
	"${CPP_MCP_ROOT}/src/mcp_resource.cpp"
	"${CPP_MCP_ROOT}/src/mcp_server.cpp"
	"${CPP_MCP_ROOT}/src/mcp_tool.cpp"
)

target_compile_features(cpp-mcp PRIVATE cxx_std_17)

# Patched mirror FIRST so its mcp_message.h / mcp_server.h win over the submodule's;
# common/ supplies httplib.h (no shared-ABI concern there).
target_include_directories(cpp-mcp PUBLIC
	"${CPP_MCP_PATCHED_INC}"
	"${CPP_MCP_ROOT}/common"
)

target_compile_definitions(cpp-mcp PUBLIC
	MCP_MAX_SESSIONS=10
	MCP_SESSION_TIMEOUT=30
	_WINSOCKAPI_
	_CRT_SECURE_NO_WARNINGS
)

target_link_libraries(cpp-mcp PUBLIC nlohmann_json::nlohmann_json ws2_32 crypt32)

if(MSVC)
	# Third-party: do not hold it to devbench's /W4 /WX.
	target_compile_options(cpp-mcp PRIVATE /utf-8 /bigobj /W0)
endif()
