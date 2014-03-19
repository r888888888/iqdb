#ifndef AUTO_CLEAN_H
#define AUTO_CLEAN_H

/***************************************************************************\
    Design patterns to automatically clean up when a variable goes out of scope.

    Copyright (C) 2008 piespy@gmail.com

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
\**************************************************************************/

/* 1. Object version. Call given member function when going out of scope.
      For when always cleaning in the destructor is not desired.

   Example:
   typedef AutoClean<Foo, &Foo::clean> CleanFoo;

   Calls Foo::clean when the CleanFoo object goes out of scope.

   2. Pointer version. Deletes pointee when going out of scope and trusts
      destructor to do the cleaning.

   Example:
   typedef AutoCleanPtr<Foo> CleanFoo;

   Also supports object.set(foo2) to delete the old pointer (if any) and use
   the new one (which may be NULL), and object.detach() which releases control
   over the pointer and returns it, NOT deleting it.

   2b. Function version. Calls second template argument function instead of delete.

   2c. Array version. Uses delete[] instead of delete.
*/

#include <stdexcept>

template<typename T, void (T::*cleanup_func)()>
class AutoClean : public T {
public:
	AutoClean() { }
	AutoClean(const T& v) : T(v) { }
	~AutoClean() { (this->*cleanup_func)(); }

private:
	AutoClean(const AutoClean&);
	AutoClean& operator = (const AutoClean&);
};

template<typename T, void (*cleanup_func)(T&)>
class AutoCleanF : public T {
public:
	AutoCleanF() { }
	AutoCleanF(const T& v) : T(v) { }
	~AutoCleanF() { (*cleanup_func)(*static_cast<T*>(this)); }

	T* operator&() { return static_cast<T*>(this); }

private:
	AutoCleanF(const AutoCleanF&);
	AutoCleanF& operator = (const AutoCleanF&);
};

template<typename T>
class AutoCleanPtr {
public:
	AutoCleanPtr() : m_p(NULL) { }
	AutoCleanPtr(T* p) : m_p(p) { }
	~AutoCleanPtr() { set(NULL); }

	T* set(T* p) { T* old = m_p; delete m_p; m_p = p; return old; }
	T* detach() { T* old = m_p; m_p = NULL; return old; }

	operator T* () { return m_p; }
	operator const T* () const { return m_p; }

	T& operator* () { return *m_p; }
	T* operator->() { return m_p; }
	bool operator!(){ return !m_p; }

private:
	AutoCleanPtr(const AutoCleanPtr&);
	AutoCleanPtr& operator = (const AutoCleanPtr&);

	T* m_p;
};

template<typename T, void (*cleanup_func)(T*)>
class AutoCleanPtrF {
public:
	AutoCleanPtrF() : m_p(NULL) { }
	AutoCleanPtrF(T* p) : m_p(p) { }
	AutoCleanPtrF(const AutoCleanPtrF& other) : m_p(NULL) { if (other.m_p) throw std::runtime_error("Bad AutoCleanPtrF copy."); }
	~AutoCleanPtrF() { set(NULL); }

	T* set(T* p) { T* old = m_p; if (m_p) cleanup_func(m_p); m_p = p; return old; }
	T* detach() { T* old = m_p; m_p = NULL; return old; }

	operator T* () { return m_p; }
	operator const T* () const { return m_p; }

	T& operator* () { return *m_p; }
	T* operator->() { return m_p; }
	bool operator!(){ return !m_p; }

private:
	AutoCleanPtrF& operator = (const AutoCleanPtrF&);

	T* m_p;
};

template<typename T>
class AutoCleanArray {
public:
	AutoCleanArray() : m_p(NULL) { }
	AutoCleanArray(T* p) : m_p(p) { }
	explicit AutoCleanArray(size_t count) : m_p(new T[count]) { }
	~AutoCleanArray() { delete[] m_p; m_p = NULL; }

	T* set(T* p) { T* old = m_p; delete[] m_p; m_p = p; return old; }
	T* set(size_t count) { T* old = m_p; delete[] m_p; m_p = new T[count]; return old; }
	T* detach() { T* old = m_p; m_p = NULL; return old; }
	//operator T* () { return m_p; }
	T* ptr() { return m_p; }

	T& operator[](size_t ind) { return m_p[ind]; }

	T& operator* () { return *m_p; }
	T* operator->() { return m_p; }

private:
	AutoCleanArray(const AutoCleanArray&);
	AutoCleanArray& operator = (const AutoCleanArray&);

	T* m_p;
};

#endif // AUTO_CLEAN_H
