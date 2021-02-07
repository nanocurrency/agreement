#include "agreement.hpp"

#include "gtest/gtest.h"

#include <atomic>
#include <array>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>

namespace
{
std::chrono::milliseconds W { 50 };
std::chrono::milliseconds one { 1 };
std::random_device r;
std::default_random_engine e1(r());
std::uniform_int_distribution<uint64_t> uniform_dist(0, std::numeric_limits<uint64_t>::max ());
size_t constexpr regression_count = 200'000;
}

class fixed_validators
{
	std::unordered_map<unsigned, unsigned> items;

public:
	using key_type = decltype (items)::key_type;
	using mapped_type = decltype (items)::mapped_type;
	fixed_validators (std::initializer_list<decltype (items)::value_type> const & list) :
	items{ list }
	{
	}
	unsigned weight (unsigned const & validator) const
	{
		unsigned result = 0;
		auto existing = items.find (validator);
		if (existing != items.end ())
		{
			result = existing->second;
		}
		return result;
	}
	unsigned quorum () const
	{
		return 667;
	}
};

class uniform_validators
{
	size_t count;

public:
	using key_type = unsigned;
	using mapped_type = unsigned;
	uniform_validators (size_t count) :
	count{ count }
	{
	}
	unsigned weight (unsigned const & validator) const
	{
		assert (validator < count);
		return 1;
	}
	unsigned quorum () const
	{
		return count - ((count - 1) / 3);
	}
	size_t size () const
	{
		return count;
	}
};

class incrementing_clock
{
public:
	class time_point
	{
	public:
		bool operator< (time_point const & other) const
		{
			return value < other.value;
		}
		bool operator<= (time_point const & other) const
		{
			return value <= other.value;
		}
		bool operator== (time_point const & other) const
		{
			return value == other.value;
		}
		bool operator!= (time_point const & other) const
		{
			return !(*this == other);
		}
		time_point operator++ (int)
		{
			time_point result{ value++ };
			return result;
		}
		time_point operator+ (std::chrono::milliseconds item) const
		{
			return time_point{ value + item.count () };
		}
		time_point operator- (std::chrono::milliseconds item) const
		{
			return time_point{ value - item.count () };
		}
		std::chrono::milliseconds operator- (time_point const & other) const
		{
			return std::chrono::milliseconds{ value - other.value };
		}
		static time_point min ()
		{
			return time_point{};
		}
		static time_point max ()
		{
			return time_point{ std::numeric_limits<uint64_t>::max () };
		}
		std::chrono::milliseconds time_since_epoch () const
		{
			return std::chrono::milliseconds{ value - incrementing_clock::epoch.value };
		}
		uint64_t value{ 1000 };
	};
	static time_point constexpr epoch{ 1000 };
	inline static time_point current{ epoch + std::chrono::milliseconds{ 100'000 } };
	inline static std::uniform_int_distribution<uint64_t> dist{ 0, 1 };
	static void reset ()
	{
		current = epoch;
	}
	static time_point now ()
	{
		auto result = current++;
		return result;
	}
};
using agreement_t = nano::agreement<float, fixed_validators, incrementing_clock>;
using agreement_u_t = nano::agreement<float, uniform_validators, incrementing_clock>;
static incrementing_clock::time_point min = incrementing_clock::time_point::min ();
static incrementing_clock::time_point max = incrementing_clock::time_point::max ();

template<typename T>
void filedump (T & agreement, typename T::validators const & validators, std::filesystem::path const & path)
{
	class T::tally tally;
	std::ofstream edge_file;
	edge_file.open (path, std::ios::out | std::ios::trunc);
	auto edges = [&edge_file] (typename T::time_point const & time, std::unordered_map<typename T::object, typename T::weight> const & totals) {
		for (auto const & i: totals)
		{
			edge_file << std::to_string (time.time_since_epoch ().count ()) << ',' << std::to_string (i.first) << ',' << std::to_string (i.second) << '\n';
		}
	};
	agreement.scan (tally, typename T::time_point{}, T::time_point::max (), validators, edges);
}

TEST (consensus_validator, non_convertable)
{
	uniform_validators validators{ 3 };
	nano::agreement<std::string, uniform_validators> consensus{ W, "" };
}

TEST (consensus_slate, construction)
{
	class agreement_u_t::tally tally;
	ASSERT_TRUE (tally.empty ());
	ASSERT_EQ (0, tally.total ());
	tally.clear ();
}

// Test a single pulse
TEST (consensus_slate, insert_erase)
{
	uniform_validators validators{ 3 };
	class agreement_u_t::tally tally;
	auto const & [weight1, object1] = tally.max ();
	ASSERT_EQ (0, weight1);
	ASSERT_EQ (0.0, object1);
	auto now = incrementing_clock::now ();
	ASSERT_EQ (0, tally.total ());
	tally.rise (now, 0, 1.0, validators);
	ASSERT_EQ (1, tally.total ());
	auto const & [weight2, object2] = tally.max ();
	ASSERT_EQ (1, weight2);
	ASSERT_EQ (1.0, object2);
	tally.fall (now, 0, 1.0);
	ASSERT_TRUE (tally.empty ());
	ASSERT_EQ (0, tally.total ());
}

// Test 2 overlapping pulses by the same validator
TEST (consensus_slate, insert_overlap)
{
	uniform_validators validators{ 3 };
	class agreement_u_t::tally tally;
	auto now1 = incrementing_clock::now ();
	auto now2 = now1 + one;
	tally.rise (now1, 0, 1.0, validators);
	auto const & [weight1, object1] = tally.max ();
	ASSERT_EQ (1, weight1);
	ASSERT_EQ (1.0, object1);
	tally.rise (now2, 0, 1.0, validators);
	auto const & [weight2, object2] = tally.max ();
	ASSERT_EQ (1, weight2);
	ASSERT_EQ (1.0, object2);
	tally.fall (now1, 0, 1.0);
	auto const & [weight3, object3] = tally.max ();
	ASSERT_EQ (1, weight3);
	ASSERT_EQ (1.0, object3);
	tally.fall (now2, 0, 1.0);
	ASSERT_TRUE (tally.empty ());
}

// Test 2 non-overlapping pulses by the same validator
TEST (consensus_slate, insert_contigious)
{
	uniform_validators validators{ 3 };
	class agreement_u_t::tally tally;
	auto now1 = incrementing_clock::now ();
	auto now2 = incrementing_clock::now ();
	tally.rise (now1, 0, 1.0, validators);
	auto const & [weight1, object1] = tally.max ();
	ASSERT_EQ (1, weight1);
	ASSERT_EQ (1.0, object1);
	tally.fall (now1, 0, 1.0);
	auto const & [weight3, object3] = tally.max ();
	ASSERT_TRUE (tally.empty ());
	tally.rise (now2, 0, 1.0, validators);
	auto const & [weight2, object2] = tally.max ();
	ASSERT_EQ (1, weight2);
	ASSERT_EQ (1.0, object2);
	tally.fall (now2, 0, 1.0);
	ASSERT_TRUE (tally.empty ());
}

// Test 2 overlapping pulses by different validators
TEST (consensus_slate, insert_different)
{
	uniform_validators validators{ 3 };
	class agreement_u_t::tally tally;
	auto now1 = incrementing_clock::now ();
	auto now2 = incrementing_clock::now ();
	tally.rise (now1, 0, 1.0, validators);
	auto const & [weight1, object1] = tally.max ();
	ASSERT_EQ (1, weight1);
	ASSERT_EQ (1.0, object1);
	tally.rise (now2, 1, 1.0, validators);
	auto const & [weight2, object2] = tally.max ();
	ASSERT_EQ (2, weight2);
	ASSERT_EQ (1.0, object2);
	tally.fall (now1, 0, 1.0);
	auto const & [weight3, object3] = tally.max ();
	ASSERT_EQ (1, weight3);
	ASSERT_EQ (1.0, object3);
	tally.fall (now2, 1, 1.0);
	ASSERT_TRUE (tally.empty ());
}

// Test 2 overlapping pulses by the same validator but different values i.e. a fault
TEST (consensus_slate, fault)
{
	std::deque<agreement_u_t::validator> faults;
	auto fault = [&faults] (agreement_u_t::validator const & value) { faults.push_back(value); };
	uniform_validators validators{ 3 };
	class agreement_u_t::tally tally;
	auto now1 = incrementing_clock::now ();
	auto now2 = incrementing_clock::now ();
	tally.rise (now1, 0, 1.0, validators, fault);
	auto const & [weight1, object1] = tally.max ();
	ASSERT_EQ (1, weight1);
	ASSERT_EQ (1.0, object1);
	tally.rise (now2, 0, 2.0, validators, fault);
	ASSERT_EQ (1, faults.size ());
	ASSERT_EQ (0, faults[0]);
	auto const & [weight2, object2] = tally.max ();
	ASSERT_EQ (1, weight2);
	ASSERT_EQ (1.0, object2);
	tally.fall (now1, 0, 1.0);
	auto const & [weight3, object3] = tally.max ();
	ASSERT_TRUE (tally.empty ());
	tally.fall (now2, 0, 2.0);
	ASSERT_TRUE (tally.empty ());
}

TEST (consensus_slate, fault_identical)
{
	uniform_validators validators{ 3 };
	class agreement_u_t::tally tally;
	auto now1 = incrementing_clock::now ();
	auto now2 = now1 + one;
	tally.rise (now1, 0, 1.0, validators);
	tally.rise (now2, 0, 2.0, validators);
	tally.rise (now2, 0, 1.0, validators);
	tally.fall (now1, 0, 1.0);
	tally.fall (now2, 0, 2.0);
	ASSERT_FALSE (tally.empty ());
	tally.fall (now2, 0, 1.0);
	ASSERT_TRUE (tally.empty ());
}

TEST (consensus_slate, fault_covered)
{
	uniform_validators validators{ 3 };
	class agreement_u_t::tally tally;
	auto now1 = incrementing_clock::now ();
	auto now2 = incrementing_clock::now ();
	auto now3 = incrementing_clock::now ();
	tally.rise (now1, 0, 1.0, validators);
	tally.rise (now2, 0, 2.0, validators);
	tally.fall (now1, 0, 1.0);
	tally.rise (now3, 0, 2.0, validators);
	tally.fall (now2, 0, 2.0);
	tally.fall (now3, 0, 2.0);
}

// Test 2 non-overlapping pulses with different values
TEST (consensus_slate, insert_flip)
{
	uniform_validators validators{ 3 };
	class agreement_u_t::tally tally;
	auto now1 = incrementing_clock::now ();
	auto now2 = incrementing_clock::now ();
	tally.rise (now1, 0, 1.0, validators);
	auto const & [weight1, object1] = tally.max ();
	ASSERT_EQ (1, weight1);
	ASSERT_EQ (1.0, object1);
	tally.fall (now1, 0, 1.0);
	ASSERT_TRUE (tally.empty ());
	tally.rise (now2, 0, 2.0, validators);
	auto const & [weight2, object2] = tally.max ();
	ASSERT_EQ (1, weight2);
	ASSERT_EQ (2.0, object2);
	tally.fall (now2, 0, 2.0);
	ASSERT_TRUE (tally.empty ());
}

TEST (consensus_slate, insert_flip_fault)
{
	uniform_validators validators{ 3 };
	class agreement_u_t::tally tally;
	auto now1 = incrementing_clock::now ();
	auto now2 = incrementing_clock::now ();
	auto now3 = incrementing_clock::now ();
	tally.rise (now1, 0, 1.0, validators);
	auto const & [weight1, object1] = tally.max ();
	ASSERT_EQ (1, weight1);
	ASSERT_EQ (1.0, object1);
	tally.fall (now1, 0, 1.0);
	ASSERT_TRUE (tally.empty ());
	tally.rise (now2, 0, 2.0, validators);
	auto const & [weight2, object2] = tally.max ();
	ASSERT_EQ (1, weight2);
	ASSERT_EQ (2.0, object2);
	tally.rise (now3, 0, 1.0, validators);
	auto const & [weight3, object3] = tally.max ();
	ASSERT_EQ (1, weight3);
	ASSERT_EQ (2.0, object3);
	tally.fall (now2, 0, 2.0);
	ASSERT_TRUE (tally.empty ());
}

TEST (consensus_scan, empty)
{
	uniform_validators validators{ 3 };
	agreement_u_t agreement{ W, 0.0 };
	class agreement_u_t::tally tally;
	std::deque<std::tuple<incrementing_clock::time_point, std::unordered_map<float, unsigned>>> edges;
	agreement.scan (tally, incrementing_clock::time_point{}, incrementing_clock::time_point::max (), validators, [&edges] (incrementing_clock::time_point const & time, std::unordered_map<float, unsigned> const & totals) { edges.push_back (std::make_tuple (time, totals)); });
	ASSERT_EQ (0, edges.size ());
}

TEST (consensus_scan, one)
{
	uniform_validators validators{ 3 };
	agreement_u_t agreement{ W, 0.0 };
	auto now = incrementing_clock::now ();
	agreement.insert (1.0f, now, 0);
	class agreement_u_t::tally tally;
	std::deque<std::tuple<incrementing_clock::time_point, std::unordered_map<float, unsigned>>> edges;
	agreement.scan (tally, incrementing_clock::time_point{}, incrementing_clock::time_point::max (), validators, [&edges] (incrementing_clock::time_point const & time, std::unordered_map<float, unsigned> const & totals) { edges.push_back (std::make_tuple (time, totals)); });
	ASSERT_EQ (2, edges.size ());
	auto const &[time0, totals0] = edges [0];
	auto const &[time1, totals1] = edges [1];
	ASSERT_EQ (now, time0);
	ASSERT_EQ (1, totals0.size ());
	auto existing1 = totals0.find (1.0f);
	ASSERT_NE (totals0.end (), existing1);
	ASSERT_EQ (1, existing1->second);
	ASSERT_EQ (now + W, time1);
	ASSERT_EQ (1, totals1.size ());
	auto existing2 = totals1.find (1.0f);
	ASSERT_EQ (0, existing2->second);
}

TEST (consensus_scan, two_same_value)
{
	uniform_validators validators{ 3 };
	agreement_u_t agreement{ W, 0.0 };
	auto now1 = incrementing_clock::now ();
	auto now2 = incrementing_clock::now ();
	agreement.insert (1.0f, now1, 0);
	agreement.insert (1.0f, now2, 1);
	class agreement_u_t::tally tally;
	std::deque<std::tuple<incrementing_clock::time_point, std::unordered_map<float, unsigned>>> edges;
	agreement.scan (tally, incrementing_clock::time_point{}, incrementing_clock::time_point::max (), validators, [&edges] (incrementing_clock::time_point const & time, std::unordered_map<float, unsigned> const & totals) { edges.push_back (std::make_tuple (time, totals)); });
	ASSERT_EQ (4, edges.size ());
	auto const &[time0, totals0] = edges [0];
	auto const &[time1, totals1] = edges [1];
	auto const &[time2, totals2] = edges [2];
	auto const &[time3, totals3] = edges [3];

	ASSERT_EQ (now1, time0);
	ASSERT_EQ (1, totals0.size ());
	auto existing1 = totals0.find (1.0f);
	ASSERT_NE (totals0.end (), existing1);
	ASSERT_EQ (1, existing1->second);

	ASSERT_EQ (now2, time1);
	ASSERT_EQ (1, totals1.size ());
	auto existing2 = totals1.find (1.0f);
	ASSERT_NE (totals1.end (), existing2);
	ASSERT_EQ (2, existing2->second);
	
	ASSERT_EQ (now1 + W, time2);
	ASSERT_EQ (1, totals2.size ());
	auto existing3 = totals2.find (1.0f);
	ASSERT_NE (totals2.end (), existing3);
	ASSERT_EQ (1, existing3->second);

	ASSERT_EQ (now2 + W, time3);
	ASSERT_EQ (1, totals3.size ());
	auto existing4 = totals3.find (1.0f);
	ASSERT_NE (totals3.end (), existing4);
	ASSERT_EQ (0, existing4->second);
}

TEST (consensus_scan, two_different_value)
{
	uniform_validators validators{ 3 };
	agreement_u_t agreement{ W, 0.0 };
	auto now1 = incrementing_clock::now ();
	auto now2 = incrementing_clock::now ();
	agreement.insert (1.0f, now1, 0);
	agreement.insert (2.0f, now2, 1);
	class agreement_u_t::tally tally;
	std::deque<std::tuple<incrementing_clock::time_point, std::unordered_map<float, unsigned>>> edges;
	agreement.scan (tally, incrementing_clock::time_point{}, incrementing_clock::time_point::max (), validators, [&edges] (incrementing_clock::time_point const & time, std::unordered_map<float, unsigned> const & totals) {
		edges.push_back (std::make_tuple (time, totals));
	});
	ASSERT_EQ (4, edges.size ());
	auto const &[time0, totals0] = edges [0];
	auto const &[time1, totals1] = edges [1];
	auto const &[time2, totals2] = edges [2];
	auto const &[time3, totals3] = edges [3];

	ASSERT_EQ (now1, time0);
	ASSERT_EQ (1, totals0.size ());
	auto existing1 = totals0.find (1.0f);
	ASSERT_NE (totals0.end (), existing1);
	ASSERT_EQ (1, existing1->second);

	ASSERT_EQ (now2, time1);
	ASSERT_EQ (2, totals1.size ());
	auto existing2 = totals1.find (1.0f);
	ASSERT_NE (totals1.end (), existing2);
	ASSERT_EQ (1, existing2->second);
	auto existing3 = totals1.find (2.0f);
	ASSERT_NE (totals1.end (), existing3);
	ASSERT_EQ (1, existing3->second);
	
	ASSERT_EQ (now1 + W, time2);
	ASSERT_EQ (2, totals2.size ());
	auto existing4 = totals2.find (1.0f);
	ASSERT_NE (totals2.end (), existing4);
	ASSERT_EQ (0, existing4->second);
	auto existing5 = totals2.find (2.0f);
	ASSERT_NE (totals2.end (), existing5);
	ASSERT_EQ (1, existing5->second);
	
	ASSERT_EQ (now2 + W, time3);
	ASSERT_EQ (2, totals3.size ());
	auto existing6 = totals3.find (1.0f);
	ASSERT_NE (totals3.end (), existing6);
	ASSERT_EQ (0, existing6->second);
	auto existing7 = totals3.find (2.0f);
	ASSERT_NE (totals3.end (), existing7);
	ASSERT_EQ (0, existing7->second);
}

TEST (consensus_scan, two_same_time)
{
	uniform_validators validators{ 3 };
	agreement_u_t agreement{ W, 0.0 };
	auto now1 = incrementing_clock::now ();
	agreement.insert (1.0f, now1, 0);
	agreement.insert (1.0f, now1, 1);
	class agreement_u_t::tally tally;
	std::deque<std::tuple<incrementing_clock::time_point, std::unordered_map<float, unsigned>>> edges;
	agreement.scan (tally, incrementing_clock::time_point{}, incrementing_clock::time_point::max (), validators, [&edges] (incrementing_clock::time_point const & time, std::unordered_map<float, unsigned> const & totals) { edges.push_back (std::make_tuple (time, totals)); });
	ASSERT_EQ (2, edges.size ());
	auto const &[time0, totals0] = edges [0];
	auto const &[time1, totals1] = edges [1];
	
	ASSERT_EQ (now1, time0);
	ASSERT_EQ (1, totals0.size ());
	auto existing0 = totals0.find (1.0f);
	ASSERT_NE (totals0.end (), existing0);
	ASSERT_EQ (2, existing0->second);
	
	ASSERT_EQ (now1 + W, time1);
	ASSERT_EQ (1, totals1.size ());
	auto existing2 = totals1.find (1.0f);
	ASSERT_NE (totals1.end (), existing2);
	ASSERT_EQ (0, existing2->second);
}

TEST (consensus_scan, one_file)
{
	uniform_validators validators{ 5 };
	agreement_u_t agreement{ W, 0.0 };
	auto now1 = incrementing_clock::now ();
	auto now2 = incrementing_clock::now ();
	auto now3 = incrementing_clock::now ();
	agreement.insert (1.0f, now1, 0);
	agreement.insert (2.0f, now2, 1);
	agreement.insert (3.0f, now2, 2);
	agreement.insert (2.0f, now3, 3);
	agreement.insert (3.0f, now3, 4);
	filedump (agreement, validators, "edges.csv");
 }

TEST (consensus_validator, construction)
{
	// Test basic consensus object construction
	agreement_u_t consensus{ W, 0.0 };
}

TEST (consensus_validator, tally_1_fail)
{
	// Test that insufficient quorum does not confirm
	std::deque<agreement_u_t::validator> faults;
	auto fault = [&faults] (agreement_u_t::validator const & fault) { faults.push_back (fault); };
	auto now = incrementing_clock::now ();
	uniform_validators validators{ 3 };
	std::optional<agreement_u_t::object> agreement;
	auto confirm = [&agreement] (agreement_u_t::object const & value, unsigned const &) { agreement = value; };
	agreement_u_t consensus{ W, 0.0 };
	consensus.insert (0.0, now, 0);
	consensus.tally (now, now, validators, confirm, fault);
	ASSERT_FALSE (agreement.has_value ());
	ASSERT_TRUE (faults.empty ());
}

TEST (consensus_validator, tally_double_fail)
{
	// Test that a vote received twice will not confirm
	// The two votes, if from different validators, would confirm
	std::deque<agreement_u_t::validator> faults;
	auto fault = [&faults] (agreement_u_t::validator const & fault) { faults.push_back (fault); };
	auto now = incrementing_clock::now ();
	uniform_validators validators{ 3 };
	std::optional<agreement_u_t::object> agreement;
	auto confirm = [&agreement] (agreement_u_t::object const & value, unsigned const &) { agreement = value; };
	agreement_u_t consensus{ W, 0.0 };
	consensus.insert (0.0, now, 0);
	consensus.tally (now, now, validators, confirm, fault);
	ASSERT_FALSE (agreement.has_value ());
	// If this vote came from validator 1/2 it would confirm
	consensus.insert (0.0, now, 0);
	consensus.tally (now, now, validators, confirm, fault);
	ASSERT_FALSE (agreement.has_value ());
	ASSERT_TRUE (faults.empty ());
}

TEST (consensus_validator, tally_2_fault)
{
	// Test that byzantine faults are detected
	std::deque<agreement_u_t::validator> faults;
	auto fault = [&faults] (agreement_u_t::validator const & fault) { faults.push_back (fault); };
	auto now = incrementing_clock::now ();
	uniform_validators validators{ 3 };
	std::optional<agreement_u_t::object> agreement;
	auto confirm = [&agreement] (agreement_u_t::object const & value, unsigned const &) { agreement = value; };
	auto root = std::make_shared<agreement_u_t> (W, 0.0);
	agreement_u_t consensus{ W, 0.0, root };
	consensus.insert (0.0, now, 0);
	consensus.tally (now, now, validators, confirm, fault);
	ASSERT_FALSE (agreement.has_value ());
	// This vote violates consensus rule to space out contradicting votes
	consensus.insert (1.0, now, 0);
	consensus.tally (now, now, validators, confirm, fault);
	ASSERT_FALSE (agreement.has_value ());
	ASSERT_EQ (1, faults.size ());
}

TEST (consensus_validator, tally_multi_fault)
{
	// Test that byzantine faults are detected
	std::deque<agreement_u_t::validator> faults;
	auto fault = [&faults] (agreement_u_t::validator const & fault) { faults.push_back (fault); };
	auto now = incrementing_clock::now ();
	uniform_validators validators{ 3 };
	std::optional<agreement_u_t::object> agreement;
	auto confirm = [&agreement] (agreement_u_t::object const & value, unsigned const &) { agreement = value; };
	auto root = std::make_shared<agreement_u_t> (W, 0.0);
	agreement_u_t consensus{ W, 0.0, root };
	consensus.insert (0.0, now, 0);
	consensus.tally (now, now, validators, confirm, fault);
	ASSERT_FALSE (agreement.has_value ());
	consensus.insert (1.0, now, 0);
	consensus.tally (now, now, validators, confirm, fault);
	ASSERT_FALSE (agreement.has_value ());
	ASSERT_EQ (1, faults.size ());
	consensus.insert (2.0, now + W, 0);
	consensus.tally (now + W, now + W, validators, confirm, fault);
	ASSERT_EQ (1, faults.size ());
	consensus.insert (3.0, now + one, 0);
	consensus.tally (now, now + one, validators, confirm, fault);
	ASSERT_FALSE (agreement.has_value ());
	ASSERT_EQ (3, faults.size ());
}

TEST (consensus_validator, tally_1_succeed)
{
	// Test that a single vote of sufficient quorum will confirm
	auto now = incrementing_clock::now ();
	uniform_validators validators{ 1 };
	std::optional<agreement_u_t::object> agreement;
	auto confirm = [&agreement] (agreement_u_t::object const & value, unsigned const &) { agreement = value; };
	auto root = std::make_shared<agreement_u_t> (W, 0.0);
	agreement_u_t consensus{ W, 0.0, root };
	consensus.insert (0.0, now, 0);
	consensus.tally (min, max, validators, confirm);
	ASSERT_TRUE (agreement.has_value ());
	ASSERT_EQ (0.0, agreement.value ());
}

TEST (consensus_validator, tally_2_fail)
{
	// Test that two votes from different validators, yet insufficient quorum, will not confirm
	auto now = incrementing_clock::now ();
	// Two votes is insufficient quorum for 4 validators
	uniform_validators validators{ 4 };
	std::optional<agreement_u_t::object> agreement;
	auto confirm = [&agreement] (agreement_u_t::object const & value, unsigned const &) { agreement = value; };
	auto root = std::make_shared<agreement_u_t> (W, 0.0);
	agreement_u_t consensus{ W, 0.0, root };
	consensus.insert (0.0, now, 0);
	consensus.tally (now, now, validators, confirm);
	ASSERT_FALSE (agreement.has_value ());
	consensus.insert (0.0, now, 1);
	consensus.tally (now, now, validators, confirm);
	ASSERT_FALSE (agreement.has_value ());
}

TEST (consensus_validator, tally_2_different_fail)
{
	// Test that two votes with sufficient quorum but voting on different objects, does not confirm
	auto now = incrementing_clock::now ();
	uniform_validators validators{ 3 };
	std::optional<agreement_u_t::object> agreement;
	auto confirm = [&agreement] (agreement_u_t::object const & value, unsigned const &) { agreement = value; };
	auto root = std::make_shared<agreement_u_t> (W, 0.0);
	agreement_u_t consensus{ W, 0.0, root };
	consensus.insert (0.0, now, 0);
	consensus.tally (now, now, validators, confirm);
	ASSERT_FALSE (agreement.has_value ());
	// This vote is for a different object than the previous
	consensus.insert (1.0, now, 1);
	consensus.tally (now, now, validators, confirm);
	ASSERT_FALSE (agreement.has_value ());
}

TEST (consensus_validator, tally_2_succeed)
{
	// Test that three votes from different validators, with sufficient quorum, will confirm
	auto now = incrementing_clock::now ();
	uniform_validators validators{ 4 };
	std::optional<agreement_u_t::object> agreement;
	auto confirm = [&agreement] (agreement_u_t::object const & value, unsigned const &) { agreement = value; };
	auto root = std::make_shared<agreement_u_t> (W, 0.0);
	agreement_u_t consensus{ W, 0.0, root };
	consensus.insert (0.0, now, 0);
	consensus.insert (0.0, now, 1);
	consensus.tally (min, max, validators, confirm);
	ASSERT_FALSE (agreement.has_value ());
	consensus.insert (0.0, now, 2);
	consensus.tally (min, max, validators, confirm);
	ASSERT_TRUE (agreement.has_value ());
	ASSERT_EQ (0.0, agreement.value ());
}

TEST (consensus_validator, tally_2_time_succeed)
{
	// Test the maximum time spacing between two votes that will confirm
	auto now = incrementing_clock::now ();
	uniform_validators validators{ 4 };
	std::optional<agreement_u_t::object> agreement;
	auto confirm = [&agreement] (agreement_u_t::object const & value, unsigned const &) { agreement = value; };
	auto root = std::make_shared<agreement_u_t> (W, 0.0);
	agreement_u_t consensus{ W, 0.0, root };
	consensus.insert (0.0, now, 0);
	consensus.insert (0.0, now, 1);
	consensus.tally (min, max, validators, confirm);
	ASSERT_FALSE (agreement.has_value ());
	// Maximum slate size is W, this vote and the previous need to fit in a single slate
	consensus.insert (0.0, now + W - one, 2);
	consensus.tally (min, max, validators, confirm);
	ASSERT_TRUE (agreement.has_value ());
	ASSERT_EQ (0.0, agreement.value ());
}

TEST (consensus_validator, tally_2_time_succeed_reverse)
{
	// Test the maximum time spacing between votes, but insert them in reverse order
	auto now = incrementing_clock::now ();
	uniform_validators validators{ 4 };
	std::optional<agreement_u_t::object> agreement;
	auto confirm = [&agreement] (agreement_u_t::object const & value, unsigned const &) { agreement = value; };
	auto root = std::make_shared<agreement_u_t> (W, 0.0);
	agreement_u_t consensus{ W, 0.0, root };
	consensus.insert (0.0, now + W - one, 0);
	consensus.insert (0.0, now, 1);
	consensus.tally (min, max, validators, confirm);
	ASSERT_FALSE (agreement.has_value ());
	consensus.insert (0.0, now, 2);
	consensus.tally (min, max, validators, confirm);
	ASSERT_TRUE (agreement.has_value ());
	ASSERT_EQ (0.0, agreement.value ());
}

TEST (consensus_validator, tally_2_time_fail)
{
	// Test that two votes of sufficient quorum that do not land in any common slate, do not confirm.
	auto now = incrementing_clock::now ();
	uniform_validators validators{ 3 };
	std::optional<agreement_u_t::object> agreement;
	auto confirm = [&agreement] (agreement_u_t::object const & value, unsigned const &) { agreement = value; };
	auto root = std::make_shared<agreement_u_t> (W, 0.0);
	agreement_u_t consensus{ W, 0.0, root };
	consensus.insert (0.0, now, 0);
	consensus.tally (now, now, validators, confirm);
	ASSERT_FALSE (agreement.has_value ());
	// This vote does not have any slate in common with the previous
	consensus.insert (0.0, now + W, 1);
	consensus.tally (now + W, now + W, validators, confirm);
	ASSERT_FALSE (agreement.has_value ());
}

TEST (consensus_validator, tally_3_succeed)
{
	// Test that four votes, three of which agree and have sufficient quorum, will confirm
	auto now = incrementing_clock::now ();
	uniform_validators validators{ 4 };
	std::optional<agreement_u_t::object> agreement;
	auto confirm = [&agreement] (agreement_u_t::object const & value, unsigned const &) { agreement = value; };
	auto root = std::make_shared<agreement_u_t> (W, 0.0);
	agreement_u_t consensus{ W, 0.0, root };
	consensus.insert (0.0, now, 0);
	consensus.tally (min, max, validators, confirm);
	ASSERT_FALSE (agreement.has_value ());
	consensus.insert (1.0, now, 1);
	consensus.insert (1.0, now, 2);
	consensus.tally (min, max, validators, confirm);
	ASSERT_FALSE (agreement.has_value ());
	consensus.insert (1.0, now, 3);
	consensus.tally (min, max, validators, confirm);
	ASSERT_TRUE (agreement.has_value ());
	ASSERT_EQ (1.0, agreement.value ());
}

TEST (consensus_validator, tally_3_fail)
{
	// Test that three votes, none of which agree, does not confirm
	auto now = incrementing_clock::now ();
	uniform_validators validators{ 3 };
	std::optional<agreement_u_t::object> agreement;
	auto confirm = [&agreement] (agreement_u_t::object const & value, unsigned const &) { agreement = value; };
	auto root = std::make_shared<agreement_u_t> (W, 0.0);
	agreement_u_t consensus{ W, 0.0, root };
	consensus.insert (0.0, now, 0);
	consensus.tally (now, now, validators, confirm);
	ASSERT_FALSE (agreement.has_value ());
	consensus.insert (1.0, now, 1);
	consensus.tally (now, now, validators, confirm);
	ASSERT_FALSE (agreement.has_value ());
	consensus.insert (2.0, now, 2);
	consensus.tally (now, now, validators, confirm);
	ASSERT_FALSE (agreement.has_value ());
}

TEST (consensus_validator, hold_min)
{
	auto now = incrementing_clock::now ();
	uniform_validators validators{ 4 };
	std::optional<agreement_u_t::object> agreement;
	auto confirm = [&agreement] (agreement_u_t::object const & value, unsigned const &) { agreement = value; };
	auto root = std::make_shared<agreement_u_t> (W, 0.0);
	agreement_u_t consensus{ W, 0.0, root };
	consensus.insert (1.0, now, 0);
	consensus.insert (1.0, now, 1);
	consensus.insert (1.0, now + W, 2);
	consensus.tally (incrementing_clock::time_point::min (), incrementing_clock::time_point::max (), validators, confirm);
	ASSERT_FALSE (agreement.has_value ());
	consensus.insert (1.0, now + W - one, 3);
	consensus.tally (incrementing_clock::time_point::min (), incrementing_clock::time_point::max (), validators, confirm);
	ASSERT_TRUE (agreement.has_value ());
	ASSERT_EQ (1.0, agreement.value ());
}

TEST (consensus_validator, hold_two_fail)
{
	auto now = incrementing_clock::now ();
	uniform_validators validators{ 4 };
	std::optional<agreement_u_t::object> agreement;
	auto confirm = [&agreement] (agreement_u_t::object const & value, unsigned const &) { agreement = value; };
	auto root = std::make_shared<agreement_u_t> (W, 0.0);
	agreement_u_t consensus{ W, 0.0, root };
	consensus.insert (1.0, now, 0);
	consensus.insert (1.0, now, 1);
	consensus.insert (1.0, now + W - one, 2);
	consensus.tally (incrementing_clock::time_point::min (), incrementing_clock::time_point::max (), validators, confirm, agreement_u_t::fault_null, std::chrono::milliseconds{ 2 });
	ASSERT_FALSE (agreement.has_value ());
}


TEST (consensus_validator, hold_two_succeed)
{
	auto now = incrementing_clock::now ();
	uniform_validators validators{ 4 };
	std::optional<agreement_u_t::object> agreement;
	auto confirm = [&agreement] (agreement_u_t::object const & value, unsigned const &) { agreement = value; };
	auto root = std::make_shared<agreement_u_t> (W, 0.0);
	agreement_u_t consensus{ W, 0.0, root };
	consensus.insert (1.0, now, 0);
	consensus.insert (1.0, now, 1);
	consensus.insert (1.0, now + W - one - one, 2);
	consensus.tally (incrementing_clock::time_point::min (), incrementing_clock::time_point::max (), validators, confirm, agreement_u_t::fault_null, std::chrono::milliseconds{ 2 });
	ASSERT_TRUE (agreement.has_value ());
	ASSERT_EQ (1.0, agreement.value ());
}

TEST (consensus_validator, tally_many_succeed)
{
	auto now = incrementing_clock::now ();
	uniform_validators validators{ 1000 };
	std::optional<agreement_u_t::object> agreement;
	auto confirm = [&agreement] (agreement_u_t::object const & value, unsigned const &) { agreement = value; };
	auto root = std::make_shared<agreement_u_t> (W, 0.0);
	agreement_u_t consensus{ W, 0.0, root };
	for (auto i = 0; i < 1000; ++i)
	{
		consensus.insert (i < 667 ? 1.0 : 2.0, now, i);
	}
	consensus.tally (min, max, validators, confirm);
	ASSERT_TRUE (agreement.has_value ());
	ASSERT_EQ (1.0, agreement.value ());
}

TEST (consensus_generator, insert_one_parent)
{
	auto generator1 = std::make_shared<agreement_u_t>(W, 0.0);
	auto generator2 = std::make_shared<agreement_u_t>(W, 0.0, generator1);
}

TEST (consensus_generator, insert_two_children)
{
	auto generator1 = std::make_shared<agreement_u_t>(W, 0.0);
	auto generator2 = std::make_shared<agreement_u_t>(W, -1.0, generator1);
	auto generator3 = std::make_shared<agreement_u_t>(W, 1.0, generator1);
}

TEST (consensus_generator, insert_two_parents)
{
	auto generator1 = std::make_shared<agreement_u_t>(W, 0.0);
	auto generator2 = std::make_shared<agreement_u_t>(W, 1.0);
	std::array<std::shared_ptr<agreement_u_t>, 2> parents = { generator1, generator2 };
	auto generator3 = std::make_shared<agreement_u_t>(W, 0.5, parents.begin (), parents.end ());
}

TEST (consensus_generator, update_zero_parents)
{
	fixed_validators validators{};
	auto generator = std::make_shared<agreement_t>(W, 0.0);
	std::vector<std::pair<agreement_t::object, agreement_u_t::time_point>> values;
	auto vote = [&values] (agreement_t::object value, agreement_t::time_point time) { values.push_back (std::make_pair (value, time)); };
	auto next = generator->vote (vote, validators);
	ASSERT_EQ (1, values.size ());
	ASSERT_EQ (0.0, values[0].first);
	ASSERT_EQ (values[0].second + W, next);
}

TEST (consensus_generator, update_one_parent)
{
	fixed_validators validators{};
	auto generator1 = std::make_shared<agreement_t>(W, 0.0);
	auto generator2 = std::make_shared<agreement_t>(W, 1.0, generator1);
	std::vector<float> values;
	auto vote = [&values] (agreement_t::object value, agreement_t::time_point time) { values.push_back (value); };
	generator2->vote (vote, validators);
	ASSERT_EQ (1, values.size ());
	ASSERT_EQ (1.0, values[0]);
}

TEST (consensus_generator, update_two_children)
{
	fixed_validators validators{};
	auto generator1 = std::make_shared<agreement_t>(W, 0.0);
	auto generator2 = std::make_shared<agreement_t>(W, -1.0, generator1);
	auto generator3 = std::make_shared<agreement_t>(W, 1.0, generator1);
	std::vector<agreement_t::object > values;
	auto vote = [&values] (agreement_t::object  value, agreement_t::time_point time) { values.push_back (value); };
	generator2->vote (vote, validators);
	generator3->vote (vote, validators);
	ASSERT_EQ (2, values.size ());
	ASSERT_EQ (-1.0, values[0]);
	ASSERT_EQ (1.0, values[1]);
}

TEST (consensus_generator, update_two_parents)
{
	fixed_validators validators{};
	auto generator1 = std::make_shared<agreement_t>(W, 0.0);
	auto generator2 = std::make_shared<agreement_t>(W, 1.0);
	std::array<std::shared_ptr<agreement_t>, 2> parents { generator1, generator2 };
	auto generator3 = std::make_shared<agreement_t>(W, 0.5, parents.begin (), parents.end ());
	std::vector<agreement_t::object> values;
	auto vote = [&values] (agreement_t::object value, agreement_t::time_point time) { values.push_back (value); };
	generator3->vote (vote, validators);
	ASSERT_EQ (1, values.size ());
	ASSERT_EQ (0.5, values[0]);
}

TEST (consensus_generator, replace_single_fail)
{
	uniform_validators validators{ 4 };
	auto now = incrementing_clock::now ();
	auto generator1 = std::make_shared<agreement_u_t>(W, 0.0);
	auto generator2 = std::make_shared<agreement_u_t>(W, 1.0, generator1);
	std::vector<agreement_t::object> values;
	auto vote = [&values] (agreement_t::object value, agreement_t::time_point time) { values.push_back (value); };
	generator2->vote (vote, validators, now);
	ASSERT_EQ (1, values.size ());
	ASSERT_EQ (1.0, values[0]);
	generator2->insert (2.0, now, 0);
	generator2->insert (2.0, now, 1);
	generator2->insert (2.0, now, 2);
	auto next = generator2->vote (vote, validators, now + generator2->W - one);
	ASSERT_EQ (1, values.size ());
	//ASSERT_EQ (now + generator2->W, next);
}

TEST (consensus_generator, replace_single_succeed)
{
	uniform_validators validators{ 4 };
	auto now = incrementing_clock::now ();
	auto generator1 = std::make_shared<agreement_u_t>(W, 0.0);
	auto generator2 = std::make_shared<agreement_u_t>(W, 1.0, generator1);
	std::vector<agreement_t::object> values;
	auto vote = [&values] (agreement_t::object value, agreement_t::time_point time) { values.push_back (value); };
	generator2->vote (vote, validators, now - one);
	ASSERT_EQ (1, values.size ());
	ASSERT_EQ (1.0, values[0]);
	generator2->insert (2.0, now, 0);
	generator2->insert (2.0, now, 1);
	generator2->insert (2.0, now, 2);
	auto next = generator2->vote (vote, validators, now + generator2->W - one);
	ASSERT_EQ (2, values.size ());
	ASSERT_EQ (2.0, values[1]);
	ASSERT_EQ (now + generator2->W - one + generator2->W, next);
}

TEST (consensus_generator, multi_value)
{
	uniform_validators validators{ 3 };
	auto now = incrementing_clock::now ();
	auto generator1 = std::make_shared<agreement_u_t>(W, 0.0);
	auto generator2 = std::make_shared<agreement_u_t>(W, 1.0, generator1);
	generator2->insert (2.0, now + one, 0);
	generator2->insert (3.0, now + one, 1);
	generator2->insert (4.0, now + one, 2);
	std::vector<agreement_t::object> values;
	auto vote = [&values] (agreement_t::object value, agreement_t::time_point time) { values.push_back (value); };
	generator2->vote (vote, validators, now + generator2->W);
	ASSERT_EQ (1, values.size ());
	ASSERT_TRUE (2.0 == values[0] || 3.0 == values[0] || 4.0 == values[0]);
}

TEST (consensus_perf, create_no_parents)
{
	for (auto i = 0; i < regression_count; ++i)
	{
		agreement_t generator{ W, 0.0 };
	}
}

TEST (consensus_perf, insert_one_parent)
{
	auto generator1 = std::make_shared<agreement_t>(W, 0.0);
	for (auto i = 0; i < regression_count; ++i)
	{
		auto generator2 = std::make_shared<agreement_t>(W, 1.0, generator1);
	}
}

TEST (consensus_perf, insert_two_parents)
{
	auto generator1 = std::make_shared<agreement_t>(W, 0.0);
	auto generator2 = std::make_shared<agreement_t>(W, 1.0);
	for (auto i = 0; i < regression_count; ++i)
	{
		std::array<std::shared_ptr<agreement_t>, 2> parents{ generator1, generator2 };
		auto generator3 = std::make_shared<agreement_t>(W, 2.0, parents.begin (), parents.end ());
	}
}

TEST (consensus_perf, generate_arbitrary_2_parents)
{
	uniform_validators validators{ 1 };
	std::deque<std::shared_ptr<agreement_u_t>> inserted;
	inserted.push_back (std::make_shared<agreement_u_t>(W, -2.0));
	inserted.push_back (std::make_shared<agreement_u_t>(W, -1.0));
	for (auto i = 0; i < regression_count; ++i)
	{
		if ((i + 1) % 10'000 == 0)
		{
			for (size_t i = 0, n = inserted.size () / 2; i < n; ++i)
			{
				auto now = incrementing_clock::now ();
				inserted.front ()->tally (now, now, validators);
				inserted.pop_front ();
			}
		}
		std::array<std::shared_ptr<agreement_u_t>, 2> parents;
		std::uniform_int_distribution<uint64_t> dist (0, inserted.size () - 1);
		parents[0] = inserted[dist (e1)];
		parents[1] = inserted[dist (e1)];
		inserted.push_back (std::make_shared<agreement_u_t> (W, i, parents.begin (), parents.end ()));
	}
}

TEST (consensus_perf, generate_arbitrary_n_parents)
{
	uniform_validators validators{ 1 };
	std::deque<std::shared_ptr<agreement_u_t>> inserted;
	inserted.push_back (std::make_shared<agreement_u_t>(W, 0.0));
	for (auto i = 0; i < regression_count; ++i)
	{
		if ((i + 1) % 1000 == 0)
		{
			for (size_t i = 0, n = inserted.size () / 2; i != n; ++i)
			{
				auto now = incrementing_clock::now ();
				inserted.front ()->tally (now, now, validators);
				inserted.pop_front ();
			}
		}
		std::deque<std::shared_ptr<agreement_u_t>> parents;
		std::uniform_int_distribution<uint64_t> dist (0, inserted.size () - 1);
		for (uint64_t i = 0, n = dist (e1) ; i < n; ++i)
		{
			parents.push_back (inserted[i]);
		}
		inserted.push_back (std::make_shared<agreement_u_t> (W, i, parents.begin (), parents.end ()));
	}
}

TEST (consensus_perf, validate_duplicates)
{
	auto now = incrementing_clock::now ();
	fixed_validators validators{};
	std::optional<agreement_t::object> agreement;
	auto confirm = [&agreement] (agreement_t::object const & value, unsigned const &) { agreement = value; };
	agreement_t validator{ W, 0.0 };
	for (auto i = 0; i < regression_count; ++i)
	{
		validator.insert (0.0, now, 123);
		validator.tally (now, now, validators, confirm);
		ASSERT_FALSE (agreement.has_value ());
	}
}

using agreement_binary_t = nano::agreement<bool, uniform_validators, incrementing_clock>;

TEST (consensus_perf, validate_10)
{
	auto now = incrementing_clock::now ();
	uniform_validators validators{ 10 };
	std::optional<agreement_binary_t::object> agreement;
	auto confirm = [&agreement] (agreement_binary_t::object const & value, unsigned const &) { agreement = value; };
	agreement_binary_t validator{ W, 0 };
	for (auto i = 0; i < regression_count; ++i)
	{
		for (unsigned j = 0; !agreement.has_value () && j < 10; ++j)
		{
			validator.insert (0, now, j);
			validator.tally (now, now, validators, confirm);
		}
	}
}

TEST (consensus_perf, validate_100)
{
	auto now = incrementing_clock::now ();
	uniform_validators validators{ 100 };
	std::optional<agreement_binary_t::object> agreement;
	auto confirm = [&agreement] (agreement_binary_t::object const & value, unsigned const &) { agreement = value; };
	agreement_binary_t validator{ W, 0 };
	for (auto i = 0; i < regression_count; ++i)
	{
		for (unsigned j = 0; !agreement.has_value () && j < 100; ++j)
		{
			validator.insert (0, now, j);
			validator.tally (now, now, validators, confirm);
		}
	}
}

TEST (consensus_perf, DISABLED_validate_1000)
{
	auto now = incrementing_clock::now ();
	uniform_validators validators{ 1000 };
	std::optional<agreement_binary_t::object> agreement;
	auto confirm = [&agreement] (agreement_binary_t::object const & value, unsigned const &) { agreement = value; };
	agreement_binary_t validator{ W, 0 };
	for (auto i = 0; i < regression_count; ++i)
	{
		for (unsigned j = 0; !agreement.has_value () && j < 1000; ++j)
		{
			validator.insert (0, now, j);
			validator.tally (now, now, validators, confirm);
		}
	}
}

using agreement_short_sys_t = nano::agreement<uint16_t, uniform_validators>;

bool fuzz_body ()
{
	std::chrono::milliseconds W{ 50 };
	uniform_validators validators{ 4 };
	class shared
	{
	public:
		std::unordered_set<bool> confirmed;
		class vote
		{
		public:
			decltype(confirmed)::value_type obj;
			std::chrono::system_clock::time_point time;
			decltype(validators)::key_type validator;
		};

		std::chrono::milliseconds W;
		std::mutex mutex;
		std::deque<vote> messages;

		shared (std::chrono::milliseconds const & W) :
		W{ W }
		{
		}
		vote get ()
		{
			std::lock_guard<std::mutex> lock (mutex);
			auto bound = 4 * W;
			while (messages.front ().time < messages.back ().time - bound)
			{
				messages.pop_front ();
			}
			std::uniform_int_distribution<uint64_t> dist (0, messages.size () - 1);
			auto index = dist (e1);
			return messages[index];
		}
		void put (decltype(confirmed)::value_type const & obj, std::chrono::system_clock::time_point const & time, unsigned self)
		{
			std::lock_guard<std::mutex> lock (mutex);
			messages.push_back ({ obj, time, self });
		}
		void confirm (decltype(confirmed)::value_type value)
		{
			std::lock_guard<std::mutex> lock (mutex);
			confirmed.insert (value);
		}
	};
	std::uniform_int_distribution<uint64_t> dist (0, 1);
	class consensus
	{
	public:
		consensus (std::chrono::milliseconds const & W, uniform_validators & validators, shared & shared, unsigned self, std::atomic<size_t> & done) :
		W{ W },
		validators{ validators },
		shared{ shared },
		self{ self },
		item{ std::make_shared<agreement_short_sys_t> (W, dist (e1), std::make_shared<agreement_short_sys_t> (W, 0)) },
		add{ [this] (decltype(shared.confirmed)::value_type const & obj, std::chrono::system_clock::time_point const & time) {
			this->shared.put (obj, time, this->self);
		} },
		done{ done }
		{
		}
		bool faulty () const
		{
			return self < ((validators.size () - 1) / 3);
		}
		void vote ()
		{
			if (!faulty ())
			{
				item->vote (add, validators);
			}
			else
			{
				auto bound = 4 * W;
				std::uniform_int_distribution<uint64_t> warp (-bound.count (), bound.count ());
				add (dist (e1), std::chrono::system_clock::now () + std::chrono::milliseconds{ warp (e1) });
			}
		}
		void action ()
		{
			unsigned weight_l;
			std::lock_guard<std::mutex> lock (mutex);
			auto message = shared.get ();
			item->insert (message.obj, message.time, message.validator);
			item->tally (message.time - W + std::chrono::milliseconds{ 1 }, message.time + W, validators, [this, &weight_l] (bool const & value, unsigned const & weight) {
				weight_l = weight;
				agreement = value;
			}, agreement_short_sys_t::fault_null, std::chrono::milliseconds{ 51 });
			if (!set && agreement.has_value ())
			{
				shared.confirm (agreement.value ());
				set = true;
				++done;
			}
			auto now = std::chrono::system_clock::now ();
			if (fuzz)
			{
				last = now;
				vote ();
			}
		}
		void dump ()
		{
			if (!faulty ())
			{
				filedump (*item, validators, std::string ("edges_") + std::to_string (self) + ".csv");
			}
		}
		std::mutex mutex;
		std::chrono::milliseconds const W;
		std::uniform_int_distribution<uint64_t> dist{ 0, 1 };
		uniform_validators & validators;
		shared & shared;
		unsigned self;
		std::optional<bool> agreement;
		std::shared_ptr<agreement_short_sys_t> item;
		std::function<void(bool const & obj, std::chrono::system_clock::time_point const & time)> add;
		bool set{ false };
		std::atomic<size_t> & done;
		std::chrono::system_clock::time_point last;
		bool const fuzz{ true };
	};
	std::deque<std::thread> threads;
	std::deque<consensus> agreements;
	std::atomic<size_t> done{ 0 };
	shared shared{ W };
	for (decltype(validators)::key_type i = 0; i < validators.size (); ++i)
	{
		agreements.emplace_back (W, validators, shared, i, done);
		agreements.back ().vote ();
	}
	for (auto i = 0; i < std::thread::hardware_concurrency(); ++i)
	{
		threads.emplace_back ([&agreements, &done, &validators] () {
			std::uniform_int_distribution<uint64_t> dist{ 0, validators.size () - 1 };
			while (done < validators.size ())
			{
				agreements[dist (e1)].action ();
			}
		});
	}
	std::for_each (threads.begin (), threads.end (), [] (std::thread & thread) {
		thread.join ();
	});
	auto error = shared.confirmed.size () != 1;
	if (error)
	{
		std::for_each (agreements.begin (), agreements.end (), [] (consensus & item) {
			item.dump ();
		});
	}
	return error;
}

TEST (consensus, fuzz)
{
	int success = 0, failure = 0;
	//for (auto i = 0; i < 2000; ++i)
	while (true)
	{
		if (fuzz_body())
		{
			++failure;
		}
		else
		{
			++success;
		}
		std::cerr << success << ' ' << failure << std::endl;
	}
}
