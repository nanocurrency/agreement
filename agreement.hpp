#pragma once

#include <algorithm>
#include <cassert>
#include <chrono>
#include <map>
#include <memory>
#include <stack>
#include <unordered_map>
#include <unordered_set>

namespace nano
{
template <typename OBJ, typename VALIDATORS, typename CLOCK = std::chrono::system_clock, typename DURATION = std::chrono::milliseconds>
class agreement : public std::enable_shared_from_this<agreement<OBJ, VALIDATORS, CLOCK, DURATION>>
{
public:
	using object = OBJ;
	using validators = VALIDATORS;
	using clock = CLOCK;
	using duration = DURATION;
	using time_point = typename clock::time_point;
	using validator = typename validators::key_type;
	using weight = typename validators::mapped_type;
	duration const W;
	static void edge_null (time_point const &, std::unordered_map<object, weight> const &) {};
	static void fault_null (validator const &) {};
	static void confirm_null (object const &, weight const &) {};
private:
	std::multimap<time_point, std::pair<validator, object>> votes;
	std::unordered_set<std::shared_ptr<agreement<object, validators, clock, duration>>> parents;
	time_point time;
	object last;
	
	using child = typename decltype(parents)::value_type;
public:
	// Transforms a sequence of rising and falling edges to an ordered weighted sum map
	class tally
	{
		std::multimap<weight, object, std::greater<weight>> rank;
		std::unordered_map<object, weight> totals_m;
		std::unordered_map<validator, std::tuple<object, time_point, weight>> votes;

		using vote = typename decltype(votes)::value_type;
	private:
		weight total_m{};
		template<typename OP>
		void sort (weight const & weight, object const & object, OP op)
		{
			auto & weight_object = totals_m[object];
			auto [current, end] = rank.equal_range (weight_object);
			while (current != end&& current->second != object)
			{
				++current;
			}
			assert ((current == rank.end () && weight_object == 0) || (current->first == weight_object && current->second == object));
			if (current != end)
			{
				rank.erase (current);
			}
			auto weight_new = op (weight_object, weight);
			rank.insert (std::make_pair (weight_new, object));
			assert (totals_m.size () == rank.size ());
			weight_object = weight_new;
			total_m = op (total_m, weight);
		}
	public:
		void fall (time_point const & time, validator const & validator, object const & object)
		{
			auto & [current, time_l, weight_l] = votes[validator];
			if (time == time_l && object == current)
			{
				sort (weight_l, object, std::minus<weight> ());
				time_l = time_point{};
			}
		}
		template<typename FAULT = decltype(fault_null)>
		void rise (time_point const & time, validator const & validator, object const & object, validators const & validators, FAULT const & fault = fault_null)
		{
			auto & [current, time_l, weight_l] = votes[validator];
			if (time_l == time_point{})
			{
				current = object;
				time_l = time;
				weight_l = validators.weight (validator);
				sort (weight_l, object, std::plus<weight> ());
			}
			else if (current == object)
			{
				time_l = time;
			}
			else
			{
				fault (validator);
			}
		}
		bool empty () const
		{
			auto result = std::all_of (votes.begin (), votes.end (), [] (vote const & value) { return std::get<1>(value.second) == time_point{}; });
			return result;
		}
		std::pair<weight, object> max () const
		{
			std::pair<weight, object> result{ weight{}, object{} };
			if (!rank.empty ())
			{
				result = *rank.begin ();
			}
			return result;
		}
		weight total () const
		{
			return total_m;
		}
		decltype (totals_m) totals () const
		{
			return totals_m;
		}
		void reset ()
		{
			votes.clear ();
			totals_m.clear ();
			rank.clear ();
			total_m = weight{};
		}
	};
	
private:
	template<typename UnaryFunction>
	void for_each_ancestor (UnaryFunction f)
	{
		std::unordered_set<child> marked;
		std::stack<child> work;
		work.push (this->shared_from_this ());
		while (!work.empty ())
		{
			auto top = work.top ();
			work.pop ();
			std::for_each (top->parents.begin (), top->parents.end (), [&work, &marked] (child const & value) {
				if (marked.find (value) == marked.end ())
				{
					marked.insert (value);
					work.push (value);
				}
			});
			f (top);
		}
	}
	// Iteratively mark all ancestor agreements with the time this descendant was confirmed
	void mark (time_point const & now)
	{
		for_each_ancestor ([&now] (child const & value) { value->time = now; });
	}
	// Iteratively test all ancestors to ensure this descendant can be replaced
	time_point replaceable ()
	{
		time_point result;
		for_each_ancestor ([this, &result] (child const & value) {
			auto cutoff = value->time + W;
			result = std::max (result, cutoff);
		});
		return result;
	}
public:
	agreement (duration const & window, object const & item) :
	W{ window },
	last{ item }
	{
		static_assert(std::is_integral<weight> (), "Validator weights must be an integral type");
	}
	template<typename ParentIt>
	agreement (duration const & window, object const & item, ParentIt first, ParentIt last) :
	agreement{ window, item }
	{
		parents.insert (first, last);
	}
	agreement (duration const & window, object const & item, std::shared_ptr<agreement<object, validators, clock, duration>> parent) :
	agreement{ window, item }
	{
		parents.insert (parent);
	}
	void reset (object const & item)
	{
		time = time_point{};
		last = item;
	}
	template<typename EDGE = decltype(edge_null), typename FAULT = decltype(fault_null)>
	void scan (tally & tally, time_point const & begin, time_point const & end, validators const & validators, EDGE const & edge = edge_null, FAULT const & fault = fault_null)
	{
		auto current = votes.lower_bound (begin);
		auto lower = current;
		auto stop = votes.upper_bound (end);
		while (current != stop)
		{
			auto const & [time, value] = *current;
			auto const & [validator, object] = value;
			while (lower->first <= time - W)
			{
				auto const & [time, value] = *lower;
				auto const & [validator, object] = value;
				tally.fall (time, validator, object);
				++lower;
				if (lower == stop || lower->first != time)
				{
					edge (time + W, tally.totals ());
				}
			}
			tally.rise (time, validator, object, validators, fault);
			++current;
			if (current == stop || current->first != time)
			{
				edge (time, tally.totals ());
			}
		}
		while (lower != stop && lower->first + W < end)
		{
			auto const & [time, value] = *lower;
			auto const & [validator, object] = value;
			tally.fall (time, validator, object);
			++lower;
			if (lower == stop || lower->first != time)
			{
				edge (time + W, tally.totals ());
			}
		}
	}
	void insert (object const & item, time_point const & time, validator const & validator)
	{
		votes.emplace (std::make_pair (time, std::make_pair (validator, item)));
	}
	template<typename CONFIRM = decltype(confirm_null), typename FAULT = decltype(fault_null)>
	void tally (time_point const & begin, time_point const & end, validators const & validators, CONFIRM const & confirm = confirm_null, FAULT const & fault = fault_null, duration const & hold = duration{})
	{
		class tally tally;
		bool holding = false;
		time_point set;
		object obj = last;
		auto hold_sampler = [this, &obj, &hold, &holding, &set, &tally, &validators, &confirm] (time_point const & time, std::unordered_map<object, weight> const & totals) {
			auto const & [weight, object] = tally.max ();
			auto holding_new = weight >= validators.quorum ();
			if (holding && time - set >= hold)
			{
				confirm (obj, weight);
				parents.clear ();
			}
			if (!holding || obj != object)
			{
				set = time;
				obj = object;
			}
			holding = holding_new;
		};
		scan (tally, begin, end, validators, hold_sampler, fault);
	}
	template<typename VoteFunction, typename FAULT = decltype(fault_null)>
	time_point vote (VoteFunction const & vote, validators const & validators, time_point const & now = clock::now (), FAULT const & fault = fault_null)
	{
		//votes.erase (votes.begin (), votes.upper_bound (now - W));
		class tally tally;
		scan (tally, now - W, now, validators, edge_null, fault);
		auto const & [weight, object] = tally.max ();
		auto result = now + W;
		if (tally.total () >= validators.quorum () && last != object)
		{
			auto when = replaceable ();
			if (when <= now)
			{
				last = object;
				mark (now);
				vote (last, now);
			}
			else
			{
				result = when;
			}
		}
		else
		{
			mark (now);
			vote (last, now);
		}
		return result;
	}
};
}
