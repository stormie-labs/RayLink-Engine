// Copyright (c) 2003 Daniel Wallin and Arvid Norberg

// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
// ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
// TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
// PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
// SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
// ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
// ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
// OR OTHER DEALINGS IN THE SOFTWARE.

#pragma once

#include <luabind/prefix.hpp>
#include <luabind/config.hpp>

#ifndef LUABIND_NO_EXCEPTIONS
#    include <exception>
#endif

struct lua_State;

namespace luabind
{

#ifndef LUABIND_NO_EXCEPTIONS

	// this exception usually means that the lua function you called
	// from C++ failed with an error code. You will have to
	// read the error code from the top of the lua stack
	// the reason why this exception class doesn't contain
	// the message itself is that string_class's copy constructor
	// may throw, if the copy constructor of an exception that is
	// being thrown throws another exception, terminate will be called
	// and the entire application is killed.
	class error : public std::exception
	{
	public:
		error(lua_State* L): m_L(L) {}
		lua_State* state() const throw() { return m_L; }
		virtual const char* what() const throw()
		{
			return "lua runtime error";
		}
	private:
		lua_State* m_L;
	};

	// if an object_cast<>() fails, this is thrown
	// it is also thrown if the return value of
	// a lua function cannot be converted
	class cast_failed : public std::exception
	{
	public:
		cast_failed(lua_State* L, LUABIND_TYPE_INFO i): m_L(L), m_info(i) {}
		lua_State* state() const throw() { return m_L; }
		LUABIND_TYPE_INFO info() const throw() { return m_info; }
		virtual const char* what() const throw() { return "unable to make cast"; }
	private:
		lua_State* m_L;
		LUABIND_TYPE_INFO m_info;
	};

#else

	typedef void(*error_callback_fun)(lua_State*);
	typedef void(*cast_failed_callback_fun)(lua_State*, LUABIND_TYPE_INFO);

    LUABIND_API void set_error_callback(error_callback_fun e);
    LUABIND_API void set_error_callback_not_crash(error_callback_fun e);
	LUABIND_API void set_cast_failed_callback(cast_failed_callback_fun c);
    LUABIND_API error_callback_fun get_error_callback();
    LUABIND_API error_callback_fun get_error_callback_not_crash();
	LUABIND_API cast_failed_callback_fun get_cast_failed_callback();

	// Anomaly Coop (2026-11): asked by a call proxy after a failed pcall, before the error callback. True = the error (still on
	// top of the stack) was logged and is survived: the proxy drops it and returns safe_result<Ret>; false = the stock path.
	// Unset (the default, and in every module but xrGame) = always the stock path.
	typedef bool(*error_survive_callback_fun)(lua_State*);
	LUABIND_API void set_error_survive_callback(error_survive_callback_fun e);
	LUABIND_API error_survive_callback_fun get_error_survive_callback();

	namespace detail
	{
		// After a failed pcall: true = survived (the error object is still on top); false = the error callback has run (it does
		// not return in a release build: Debug.fatal), exactly the stock path.
		inline bool pcall_error_survived(lua_State* L)
		{
			if (error_survive_callback_fun s = get_error_survive_callback())
				if (s(L))
					return true;
			if (error_callback_fun e = get_error_callback())
				e(L);
			return false;
		}
	}

#endif

	namespace detail
	{
		// What a call proxy returns for a survived error: nothing for void (no use), false / 0 / a zeroed value for the rest,
		// "" for a string, nullptr for a pointer; a luabind::object holding nil (its specialisation is in object.hpp).
		template<class T> struct safe_result { static T get(lua_State*) { return T(); } };
		template<> struct safe_result<const char*> { static const char* get(lua_State*) { return ""; } };
	}

	typedef int(*pcall_callback_fun)(lua_State*);
	LUABIND_API void set_pcall_callback(pcall_callback_fun e);
	LUABIND_API pcall_callback_fun get_pcall_callback();

	typedef void(*pregister_callback_fun)(lua_State*,bool);
	LUABIND_API void set_pregister_callback(pregister_callback_fun e);
	LUABIND_API pregister_callback_fun get_pregister_callback();
}
