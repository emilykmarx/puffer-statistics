#include <cstdlib>
#include <stdexcept>
#include <iostream>
#include <vector>
#include <string>
#include <array>
#include <tuple>
#include <charconv>
#include <map>
#include <cstring>
#include <fstream>
#include <google/sparse_hash_map>
#include <google/dense_hash_map>
#include <boost/container_hash/hash.hpp>

#include <jsoncpp/json/json.h>

#include <sys/time.h>
#include <sys/resource.h>

using namespace std;
using namespace std::literals;
using google::sparse_hash_map;
using google::dense_hash_map;

size_t memcheck() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) < 0) {
	perror("getrusage");
	throw runtime_error(string("getrusage: ") + strerror(errno));
    }

    if (usage.ru_maxrss > 12 * 1024 * 1024) {
	throw runtime_error("memory usage is at " + to_string(usage.ru_maxrss) + " KiB");
    }

    return usage.ru_maxrss;
}

void split_on_char(const string_view str, const char ch_to_find, vector<string_view> & ret) {
    ret.clear();

    bool in_double_quoted_string = false;
    unsigned int field_start = 0;
    for (unsigned int i = 0; i < str.size(); i++) {
	const char ch = str[i];
	if (ch == '"') {
	    in_double_quoted_string = !in_double_quoted_string;
	} else if (in_double_quoted_string) {
	    continue;
	} else if (ch == ch_to_find) {
	    ret.emplace_back(str.substr(field_start, i - field_start));
	    field_start = i + 1;
	}
    }

    ret.emplace_back(str.substr(field_start));
}

uint64_t to_uint64(string_view str) {
    uint64_t ret = -1;
    const auto [ptr, ignore] = from_chars(str.data(), str.data() + str.size(), ret);
    if (ptr != str.data() + str.size()) {
	str.remove_prefix(ptr - str.data());
	throw runtime_error("could not parse as integer: " + string(str));
    }

    return ret;
}

float to_float(const string_view str) {
    /* sadly, g++ 8 doesn't seem to have floating-point C++17 from_chars() yet
    float ret;
    const auto [ptr, ignore] = from_chars(str.data(), str.data() + str.size(), ret);
    if (ptr != str.data() + str.size()) {
	throw runtime_error("could not parse as float: " + string(str));
    }

    return ret;
    */

    /* apologies for this */
    char * const null_byte = const_cast<char *>(str.data() + str.size());
    char old_value = *null_byte;
    *null_byte = 0;

    const float ret = atof(str.data());

    *null_byte = old_value;

    return ret;
}

template <typename T>
T influx_integer(const string_view str) {
    if (str.back() != 'i') {
	throw runtime_error("invalid influx integer: " + string(str));
    }
    const uint64_t ret_64 = to_uint64(str.substr(0, str.size() - 1));
    if (ret_64 > numeric_limits<T>::max()) {
	throw runtime_error("can't convert to uint32_t: " + string(str));
    }
    return static_cast<T>(ret_64);
}

constexpr uint8_t SERVER_COUNT = 64;

uint64_t get_server_id(const vector<string_view> & fields) {
    uint64_t server_id = -1;
    for (const auto & field : fields) {
	if (not field.compare(0, 10, "server_id="sv)) {
	    server_id = to_uint64(field.substr(10)) - 1;
	}
    }

    if (server_id >= SERVER_COUNT) {
	for ( const auto & x : fields ) { cerr << "field=" << x << " "; };
	throw runtime_error( "Invalid or missing server id" );
    }

    return server_id;
}

class username_table {
    uint32_t next_id_ = 0;

    dense_hash_map<string, uint32_t> forward_{};
    dense_hash_map<uint32_t, string> reverse_{};

public:
    username_table() {
	forward_.set_empty_key({});
	reverse_.set_empty_key(-1);
    }

    uint32_t forward_map_vivify(const string & name) {
	auto ref = forward_.find(name);
	if (ref == forward_.end()) {	
	    forward_[name] = next_id_;
	    reverse_[next_id_] = name;
	    next_id_++;
	    ref = forward_.find(name);
	}
	return ref->second;
    }

    uint32_t forward_map(const string & name) const {
	auto ref = forward_.find(name);
	if (ref == forward_.end()) {	
	    throw runtime_error( "username " + name + " not found");
	}
	return ref->second;
    }

    const string & reverse_map(const uint32_t id) const {
	auto ref = reverse_.find(id);
	if (ref == reverse_.end()) {
	    throw runtime_error( "uid " + to_string(id) + " not found");
	}
	return ref->second;
    }
};

struct Event {
    struct EventType {
	enum class Type : uint8_t { init, startup, play, timer, rebuffer };
	constexpr static array<string_view, 5> names = { "init", "startup", "play", "timer", "rebuffer" };

	Type type;

	operator string_view() const { return names[uint8_t(type)]; }

	EventType(const string_view sv)
	    : type()
	{
	    if (sv == "timer"sv) { type = Type::timer; }
	    else if (sv == "play"sv) { type = Type::play; }
	    else if (sv == "rebuffer"sv) { type = Type::rebuffer; }
	    else if (sv == "init"sv) { type = Type::init; }
	    else if (sv == "startup"sv) { type = Type::startup; }
	    else { throw runtime_error( "unknown event type: " + string(sv) ); }
	}

	operator uint8_t() const { return static_cast<uint8_t>(type); }

	bool operator==(const EventType other) const { return type == other.type; }
	bool operator==(const EventType::Type other) const { return type == other; }
	bool operator!=(const EventType other) const { return not operator==(other); }
	bool operator!=(const EventType::Type other) const { return not operator==(other); }
    };

    optional<uint32_t> init_id{};
    optional<uint32_t> expt_id{};
    optional<uint32_t> user_id{};
    optional<EventType> type{};
    optional<float> buffer{};
    optional<float> cum_rebuf{};

    bool bad = false;

    bool complete() const {
	return init_id.has_value() and expt_id.has_value() and user_id.has_value()
	    and type.has_value() and buffer.has_value() and cum_rebuf.has_value();
    }

    template <typename T>
    void set_unique( optional<T> & field, const T & value ) {
	if (not field.has_value()) {
	    field.emplace(value);
	} else {
	    if (field.value() != value) {
		if (not bad) {
		    bad = true;
		    cerr << "error trying to set contradictory value: ";
		    cerr << "init_id=" << init_id.value_or(-1)
			 << ", expt_id=" << expt_id.value_or(-1)
			 << ", user_id=" << user_id.value_or(-1)
			 << ", type=" << (type.has_value() ? int(type.value()) : 'x')
			 << ", buffer=" << buffer.value_or(-1.0)
			 << ", cum_rebuf=" << cum_rebuf.value_or(-1.0)
			 << "\n";
		}
		//		throw runtime_error( "contradictory values: " + to_string(field.value()) + " vs. " + to_string(value) );
	    }
	}
    }

    void insert_unique(const string_view key, const string_view value, username_table & usernames ) {
	if (key == "init_id"sv) {
	    set_unique( init_id, influx_integer<uint32_t>( value ) );
	} else if (key == "expt_id"sv) {
	    set_unique( expt_id, influx_integer<uint32_t>( value ) );
	} else if (key == "user"sv) {
	    if (value.size() <= 2 or value.front() != '"' or value.back() != '"') {
		throw runtime_error("invalid username string: " + string(value));
	    }
	    set_unique( user_id, usernames.forward_map_vivify(string(value.substr(1,value.size()-2))) );
	} else if (key == "event"sv) {
	    set_unique( type, { value.substr(1,value.size()-2) } );
	} else if (key == "buffer"sv) {
	    set_unique( buffer, to_float(value) );
	} else if (key == "cum_rebuf"sv) {
	    set_unique( cum_rebuf, to_float(value) );
	} else {
	    throw runtime_error( "unknown key: " + string(key) );
	}
    }
};

using key_table = map<uint64_t, Event>;

struct Channel {
    constexpr static uint8_t COUNT = 6;

    enum class ID : uint8_t { cbs, nbc, abc, fox, univision, pbs };

    constexpr static array<string_view, COUNT> names = { "cbs", "nbc", "abc", "fox", "univision", "pbs" };

    ID id;

    constexpr Channel(const string_view sv)
	: id()
    {
	if (sv == "cbs"sv) { id = ID::cbs; }
	else if (sv == "nbc"sv) { id = ID::nbc; }
	else if (sv == "abc"sv) { id = ID::abc; }
	else if (sv == "fox"sv) { id = ID::fox; }
	else if (sv == "univision"sv) { id = ID::univision; }
	else if (sv == "pbs"sv) { id = ID::pbs; }
	else { throw runtime_error( "unknown channel: " + string(sv) ); }
    }

    constexpr Channel(const uint8_t id_int) : id(static_cast<ID>(id_int)) {}

    operator string_view() const { return names[uint8_t(id)]; }
    constexpr operator uint8_t() const { return static_cast<uint8_t>(id); }

    bool operator==(const Channel other) { return id == other.id; }
    bool operator!=(const Channel other) { return not operator==(other); }
};

Channel get_channel(const vector<string_view> & fields) {
    for (const auto & field : fields) {
	if (not field.compare(0, 8, "channel="sv)) {
	    return field.substr(8);
	}
    }

    throw runtime_error("channel missing");
}

class Parser {
private:
    username_table usernames{};
    array<array<key_table, Channel::COUNT>, SERVER_COUNT> client_buffer{};

    using session_key = tuple<uint32_t, uint32_t, uint32_t, uint8_t, uint8_t>;
    /*                        init_id,  uid,      expt_id,  server,  channel */
    dense_hash_map<session_key, vector<pair<uint64_t, const Event*>>, boost::hash<session_key>> sessions;

    unsigned int bad_count = 0;

    vector<string> experiments{};

    void read_experimental_settings_dump(const string & filename) {
	ifstream experiment_dump{ filename };
	if (not experiment_dump.is_open()) {
	    throw runtime_error( "can't open " + filename );
	}
	
	string line_storage;

	while (true) {
	    getline(experiment_dump, line_storage);
	    if (not experiment_dump.good()) {
		break;
	    }

	    const string_view line{line_storage};

	    const size_t separator = line.find_first_of(' ');
	    if (separator == line.npos) {
		throw runtime_error("can't find separator: " + line_storage);
	    }
	    const uint64_t experiment_id = to_uint64(line.substr(0, separator));
	    if (experiment_id > numeric_limits<uint16_t>::max()) {
		throw runtime_error("invalid expt_id: " + line_storage);
	    }
	    const string_view rest_of_string = line.substr(separator+1);
	    Json::Reader reader;
	    Json::Value doc;
	    reader.parse(string(rest_of_string), doc);
	    experiments.resize(experiment_id + 1);
	    string name = doc["abr_name"].asString();
	    if (name.empty()) {
		name = doc["abr"].asString();
	    }
	    experiments.at(experiment_id) = name + "/" + doc["cc"].asString();
	}
    }

public:
    Parser(const string & experiment_dump_filename)
	: sessions()
    {
	sessions.set_empty_key({0,0,0,-1,-1});

	read_experimental_settings_dump(experiment_dump_filename);
    }

    void parse_stdin() {
	ios::sync_with_stdio(false);
	string line_storage;

	unsigned int line_no = 0;

	vector<string_view> fields, measurement_tag_set_fields, field_key_value;

	while (cin.good()) {
	    if (line_no % 1000000 == 0) {
		const size_t rss = memcheck() / 1024;
		cerr << "line " << line_no / 1000000 << "M, RSS=" << rss << " MiB\n";
	    }

	    getline(cin, line_storage);
	    line_no++;

	    const string_view line{line_storage};

	    if (line.empty() or line.front() == '#') {
		continue;
	    }

	    if (line.size() > numeric_limits<uint8_t>::max()) {
		throw runtime_error("Line " + to_string(line_no) + " too long");
	    }

	    split_on_char(line, ' ', fields);
	    if (fields.size() != 3) {
		if (not line.compare(0, 15, "CREATE DATABASE"sv)) {
		    continue;
		}

		cerr << "Ignoring line with wrong number of fields: " << string(line) << "\n";
		continue;
	    }

	    const auto [measurement_tag_set, field_set, timestamp_str] = tie(fields[0], fields[1], fields[2]);

	    const uint64_t timestamp{to_uint64(timestamp_str)};

	    split_on_char(measurement_tag_set, ',', measurement_tag_set_fields);
	    if (measurement_tag_set_fields.empty()) {
		throw runtime_error("No measurement field on line " + to_string(line_no));
	    }
	    const auto measurement = measurement_tag_set_fields[0];

	    split_on_char(field_set, '=', field_key_value);
	    if (field_key_value.size() != 2) {
		throw runtime_error("Irregular number of fields in field set: " + string(line));
	    }

	    const auto [key, value] = tie(field_key_value[0], field_key_value[1]);

	    try {
		if ( measurement == "client_buffer"sv ) {
		    client_buffer[get_server_id(measurement_tag_set_fields)][get_channel(measurement_tag_set_fields)][timestamp].insert_unique(key, value, usernames);
		} else if ( measurement == "active_streams"sv ) {
		    // skip
		} else if ( measurement == "backlog"sv ) {
		    // skip
		} else if ( measurement == "channel_status"sv ) {
		    // skip
		} else if ( measurement == "client_error"sv ) {
		    // skip
		} else if ( measurement == "client_sysinfo"sv ) {
		    //		client_sysinfo[timestamp].insert_unique(key, value);
		} else if ( measurement == "decoder_info"sv ) {
		    // skip
		} else if ( measurement == "server_info"sv ) {
		    // skip
		} else if ( measurement == "ssim"sv ) {
		    // skip
		} else if ( measurement == "video_acked"sv ) {
		    //		video_acked[get_server_id(measurement_tag_set_fields)][timestamp].insert_unique(key, value);
		} else if ( measurement == "video_sent"sv ) {
		    //		video_sent[get_server_id(measurement_tag_set_fields)][timestamp].insert_unique(key, value);
		} else if ( measurement == "video_size"sv ) {
		    // skip
		} else {
		    throw runtime_error( "Can't parse: " + string(line) );
		}
	    } catch (const exception & e ) {
		cerr << "Failure on line: " << line << "\n";
		throw;
	    }
	}
    }

    void accumulate_sessions() {
	for (uint8_t server = 0; server < client_buffer.size(); server++) {
	    const size_t rss = memcheck() / 1024;
	    cerr << "server " << int(server) << "/" << client_buffer.size() << ", RSS=" << rss << " MiB\n";
	    for (uint8_t channel = 0; channel < Channel::COUNT; channel++) {
		for (const auto & [ts,event] : client_buffer[server][channel]) {
		    if (event.bad) {
			bad_count++;
			cerr << "Skipping bad data point (of " << bad_count << " total) with contradictory values.\n";
			continue;
		    }
		    if (not event.complete()) {
			throw runtime_error("incomplete event with timestamp " + to_string(ts));
		    }

		    sessions[{*event.init_id, *event.user_id, *event.expt_id, server, channel}].emplace_back(ts, &event);
		}
	    }
	}
    }

    struct EventSummary {
	bool valid{false};
	bool full_extent{true};
	string summary{};
	float time_extent{0}, total_since_startup{0}, total_since_first_play{0}, stall_since_first_play{0};
    };

    void analyze_sessions() const {
	unsigned int bad_count = 0;
	float total_extent = 0;
	float total_time_considered = 0;

	for ( auto & [key, events] : sessions ) {
	    EventSummary summary = summarize(key, events);
	    if (not summary.valid) {
		bad_count++;
	    }

	    cout << (summary.valid ? "good " : "bad ") << (summary.full_extent ? "full " : "trunc ") << summary.summary << "\n";
	    total_extent += summary.time_extent;

	    if (summary.valid) {
		total_time_considered += summary.total_since_startup;
	    }
	}

	cout << "discarded sessions: " << bad_count << "/" << sessions.size() << "\n";
	cout << "total time extent: " << total_extent / 3600.0 << " hours\n";
	cout << "total time considered: " << total_time_considered / 3600.0 << " hours " << 100 * total_time_considered / total_extent << "%\n";
    }

    static bool too_far_apart( const float a, const float b ) {
	const float diff = abs(a - b);
	const float bigger = max(a,b);
	if (bigger < 30) {
	    return diff > 10;
	} else {
	    return (diff / bigger) > (10.0 / 30.0);
	}
    }

    EventSummary summarize(const session_key & key, const vector<pair<uint64_t, const Event*>> & events) const {
	const auto & [init_id, uid, expt_id, server, channel] = key;

	const string username = usernames.reverse_map(uid);
	const string scheme = experiments.at(expt_id);

	EventSummary ret;

	const uint64_t base_time = events.front().first;

	ret.time_extent = (events.back().first - base_time) / double(1000000000);

	ret.summary = to_string(init_id) + " " + scheme + " " + to_string(init_id) + " extent=" + to_string(ret.time_extent) + " " + to_string(events.size()) + " events";

	/* find init event */
	unsigned int init_index = 0;
	while (init_index < events.size()) {
	    if (events[init_index].second->type.value() == Event::EventType::Type::init) {
		break;
	    }
	    init_index++;
	}

	if (init_index == events.size()) {
	    ret.summary += " [warning, no init event found]";
	    init_index = 0;
	} else {
	    init_index++;
	}

	float last_sample = 0;

	optional<float> play_started;
	optional<float> stall_started;
	optional<float> latest_successful_play_ts;
	optional<float> startup_delay;
	float expected_cum_rebuf = 0;

	float total_time_stalled = 0;

	for ( unsigned int i = init_index; i < events.size(); i++ ) {
	    if (not ret.full_extent) {
		break;
	    }

	    const auto & [ts, event] = events[i];

	    const float relative_time = (ts - base_time) / 1000000000.0;

	    if (relative_time - last_sample > 5.0) {
		ret.summary += " time_between_events=" + to_string(relative_time - last_sample);
		ret.full_extent = false;
		break;
	    }

	    switch (event->type.value().type) {
	    case Event::EventType::Type::init:
		ret.summary += " two init events in this session";
		return ret;
	    case Event::EventType::Type::play:
		if (not startup_delay.has_value()) {
		    ret.summary += " play without startup";
		    return ret;
		}

		if (play_started.has_value()) {
		    ret.summary += " two play events with no stall";
		    return ret;
		} else if (not stall_started.has_value()) {
		    ret.summary += " stall_started has no value";
		    return ret;
		} else {
		    play_started.emplace(relative_time);		    
		    latest_successful_play_ts.emplace(relative_time);
		    ret.summary += " stalled=" + to_string(relative_time - stall_started.value());
		    total_time_stalled += relative_time - stall_started.value();
		    stall_started.reset();
		    expected_cum_rebuf += relative_time - last_sample;
		}
		break;
	    case Event::EventType::Type::startup:
		if (startup_delay.has_value()) {
		    ret.summary += " startup after already started";
		    return ret;
		}

		play_started.emplace(relative_time);
		startup_delay.emplace(event->cum_rebuf.value());
		latest_successful_play_ts.emplace(relative_time);
		expected_cum_rebuf = relative_time;
		if (too_far_apart(expected_cum_rebuf, event->cum_rebuf.value())) {
		    ret.summary += " startup cum_rebuf expectation mismatch " + to_string(event->cum_rebuf.value()) + " vs. " + to_string(expected_cum_rebuf);
		    return ret;
		}
		ret.summary += " startup_delay=" + to_string(event->cum_rebuf.value()) + "@" + to_string(relative_time);
		break;
	    case Event::EventType::Type::timer:
		if (stall_started.has_value()) {
		    expected_cum_rebuf += relative_time - last_sample;
		} else if (play_started.has_value()) {
		    latest_successful_play_ts.emplace(relative_time);
		}

		if (too_far_apart(expected_cum_rebuf, event->cum_rebuf.value())) {
		    ret.summary += " timer cum_rebuf expectation mismatch " + to_string(event->cum_rebuf.value()) + " vs. " + to_string(expected_cum_rebuf);
		    ret.full_extent = false;
		}

		break;
	    case Event::EventType::Type::rebuffer:
		if (not startup_delay.has_value()) {
		    ret.summary += " stall without startup";
		    return ret;
		}

		if (stall_started.has_value()) {
		    ret.summary += " two stall events with no play";
		    return ret;
		} else if (not play_started.has_value()) {
		    ret.summary += " play_started has no value";
		    return ret;
		} else {
		    stall_started.emplace(relative_time);
		    ret.summary += " played=" + to_string(relative_time - play_started.value());
		    latest_successful_play_ts.emplace(relative_time); // matches stream_processor.py analysis
		    play_started.reset();
		}
	    }

	    last_sample = relative_time;
	}

	if (not latest_successful_play_ts.has_value()) {
	    ret.summary += " neverplayed";
	    return ret;
	}

	if (play_started
	    and (latest_successful_play_ts.value() > play_started.value())) {
	    ret.summary += " played=" + to_string(latest_successful_play_ts.value() - play_started.value());
	}

	float duration_from_startup_to_last_successful_play = latest_successful_play_ts.value() - startup_delay.value();
	ret.total_since_startup = latest_successful_play_ts.value();
	ret.total_since_first_play = latest_successful_play_ts.value() - startup_delay.value();
	ret.stall_since_first_play = total_time_stalled;

	if (total_time_stalled / duration_from_startup_to_last_successful_play > 0.75) {
	    ret.summary += " >75% stalled";
	    return ret;
	}

	ret.summary += " total_since_startup=" + to_string(duration_from_startup_to_last_successful_play) + " stalltime=" + to_string(total_time_stalled) + " startup=" + to_string(startup_delay.value());
	ret.valid = true;

	return ret;
    }
};

void analyze_main(const string & experiment_dump_filename) {
    Parser parser{ experiment_dump_filename };

    parser.parse_stdin();
    parser.accumulate_sessions();
    parser.analyze_sessions();
}

int main(int argc, char *argv[]) {
    try {
	if (argc <= 0) {
	    abort();
	}

	if (argc != 2) {
	    cerr << "Usage: " << argv[0] << " expt_dump [from postgres]\n";
	    return EXIT_FAILURE;
	}

	analyze_main(argv[1]);
    } catch (const exception & e) {
	cerr << e.what() << "\n";
	return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
