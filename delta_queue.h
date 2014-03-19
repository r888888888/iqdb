#ifndef DELTA_QUEUE_H
#define DELTA_QUEUE_H

/***************************************************************************\
    Variant of std::vector<size_t> that efficiently stores small differences.

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

#include <vector>

static const size_t size_t_mask = sizeof(size_t) - 1;

struct delta_value {
	delta_value() { }
	delta_value(size_t v) : full(v) { }

	void put(unsigned char v, int pos) { full |= (size_t)v << (8 * pos); }
	unsigned char get() { unsigned char v = full; full >>= 8; return v; }

	size_t full;
};

struct delta_iterator : public std::iterator<std::forward_iterator_tag, size_t> {
	delta_iterator(const delta_value* itr) : m_p(ptr(itr)), m_val(*itr), m_bval(0) { ++*this; }
	delta_iterator() { }

	delta_iterator& operator++();
	delta_iterator  operator++(int) { delta_iterator old = *this; ++*this; return old; }
	size_t operator*() const { return m_bval; }
	bool operator==(const delta_iterator& other) const { return m_p == other.m_p; }
	bool operator!=(const delta_iterator& other) const { return m_p != other.m_p; }

private:
	friend class delta_queue;
	delta_iterator(const delta_value* itr, int ind) : m_p(ptr(itr) + ind - size_t_mask) { }	// only useful for end()

	size_t itr() const { return *(const size_t*)(m_p & ~size_t_mask); }
	size_t ind() const { return m_p & size_t_mask; }

	static size_t ptr(const delta_value* itr) { return (size_t)itr & size_t_mask ? 0 : (size_t)itr; }

	size_t m_p;
	delta_value m_val;
	size_t m_bval;
};

class delta_queue {
protected:
	static const size_t mask = size_t_mask;
	typedef std::vector<delta_value> base_type;
	base_type m_base;

public:
	typedef delta_iterator iterator;
	typedef delta_iterator const_iterator;

	delta_queue() : m_base(2, 0), m_size(0), m_pos(0), m_bval(0) { if (sizeof(size_t) != sizeof(char*)) throw 42; }

	const_iterator begin() const { return const_iterator(&*m_base.begin()); }
	const_iterator end() const { return const_iterator(&*(m_base.end() - 1), m_size & mask); }

	// Reserve storage under the assumption most values will be <255 and fit in one byte.
	// Set fixed=true if you know the exact storage size required, then the size argument
	// is the storage size rather than the number of values.
	void reserve(size_t size, bool fixed = false) { m_base.reserve(fixed ? size : size * 129 / 512 + 1); }

	void push_back(size_t v);

	bool empty() { return !m_size; }
	size_t size() const { return m_size; }

	void swap(delta_queue& other);

	size_t base_capacity() const { return m_base.capacity(); }
	size_t base_size() const { return m_base.size(); }

private:
	size_t m_size;
	size_t m_pos;
	size_t m_bval;
};

inline delta_iterator& delta_iterator::operator++() {
	size_t val = m_val.get();
//fprintf(stderr, "Advancing, from base=%zd ind=%d val=%08x:%02x ", m_bval, ind(), m_val.full, val);
	m_bval += val;
	if (val == 255) {
		m_p += sizeof(size_t);
		m_bval += itr();
	}

	m_p++;
	val = (ind() != 0) - 1;
	m_val.full += val & itr();
//fprintf(stderr, "to base=%zd ind=%d val=%08x. ", m_bval, ind(), m_val.full);
	return *this;
}

inline void delta_queue::swap(delta_queue& other) {
	m_base.swap(other.m_base);
	std::swap(m_size, other.m_size);
	std::swap(m_pos, other.m_pos);
	std::swap(m_bval, other.m_bval);
}

inline void delta_queue::push_back(size_t v) {
	v -= m_bval;
	m_bval += v;

	if (v >= 255) {
		m_base.back() = v - 255;
		m_base.push_back(0);
		v = 255;
	}

//fprintf(stderr, "Writing %zd at %zd/%zd. ", v, m_pos, m_size & mask);
	m_base[m_pos].put(v, m_size++ & mask);
//fprintf(stderr, "Now size=%zd. ", m_size);

	if (!(m_size & mask)) {
		m_pos = m_base.size() - 1;
		m_base.push_back(0);
//fprintf(stderr, "Now pos=%zd. ", m_pos);
	};
}

#endif // DELTA_QUEUE_H
