#include <Wire.h>
#include <Adafruit_RGBLCDShield.h>
#include <utility/Adafruit_MCP23017.h>
#include <EEPROM.h>

Adafruit_RGBLCDShield lcd;

#define DEBUG
#define STUDENT_ID "F121584"

// Extensions
#define EXT_UDCHARS
#define EXT_FREERAM
// #define EXT_HCI
#define EXT_EEPROM
#define EXT_RECENT
#define EXT_NAMES
#define EXT_SCROLL

// Constants
#define BAUD_RATE 9600
#define WIDTH 16
#define HEIGHT 2
#define EXTENSIONS "UDCHARS,FREERAM,HCI,EEPROM,RECENT,NAMES,SCROLL"

#define RAM_MAX 2048
#define EEPROM_MAX (E2END + 1) // Taken from EEPROM.length() but as constexpr

#ifdef EXT_RECENT
#define MEMORY_CULLING 500
#endif // EXT_RECENT

// Debug Output
#ifdef DEBUG
#define log_debug(message) { \
Serial.print(F("DEBUG: ")); \
Serial.print(millis()); \
Serial.print(' '); \
Serial.print(__PRETTY_FUNCTION__); \
Serial.print(' '); \
Serial.print(__LINE__); \
Serial.print(F(" >> ")); \
Serial.println(message); \
}
#else // !DEBUG
#define log_debug(message)
#endif // DEBUG

// Helpers
#define BRACED_INIT_LIST(...) { __VA_ARGS__ }
#if defined(_MSC_VER) // Force Inline
#define INLINE inline __forceinline
#elif defined(__GNUC__) || defined(__GNUG__) || defined(__clang__)
#define INLINE inline __ATTR_GNU_INLINE__
#else
#define INLINE inline
#endif // Force Inline

// Backlight
enum Background : uint8_t {
	CLEAR = 0,
	RED = 1,
	GREEN = 2,
	YELLOW = 3,
	PURPLE = 5,
	WHITE = 7
};

// Picutures
struct Picture {
	const uint8_t pos;
	const byte* const img;
	Picture(const uint8_t pos, const byte* const img) : pos(pos), img(img) {}
	void upload() {
		byte buf[8];
		memcpy_P(buf, img, sizeof(buf));
		lcd.createChar(pos, buf);
	}
	operator uint8_t() const {
		return pos;
	}
};
#define PICTURE(name, pos, image) \
const ::byte __img_ ## name ## _ ## pos ## __ [8] PROGMEM = BRACED_INIT_LIST image; \
::Picture name {pos, __img_ ## name ## _ ## pos ## __ }

#if defined(EXT_RECENT)
#ifdef __arm__
// should use uinstd.h to define sbrk but Due causes a conflict
extern "C" char* sbrk(int incr);
#else  // __ARM__
extern char* __brkval;
#endif  // __arm__

uint16_t free_memory() {
	char top;
#ifdef __arm__
	return &top - reinterpret_cast<char*>(sbrk(0));
#elif defined(CORE_TEENSY) || (ARDUINO > 103 && ARDUINO != 151)
	return &top - __brkval;
#else  // __arm__
	return __brkval ? &top - __brkval : &top - __malloc_heap_start;
#endif  // __arm__
}
#endif // EXT_RECENT

// Constexpr Buffer Sizes
namespace csize {
	constexpr uint8_t create = 15;
	constexpr uint8_t write = 4;
	constexpr uint8_t protocol = max(create, write);
	constexpr uint8_t channel = 26;
	constexpr uint8_t desc = create + 1; // Includes null

#ifdef EXT_RECENT
	constexpr uint8_t history = 64;
	// Description Start Position
	constexpr uint8_t start_pos = 9;
#else // !EXT_RECENT
	// Description Start Position
	constexpr uint8_t start_pos = 5;
#endif // EXT_RECENT
}

// Allocators
namespace alloc {
	// Typed Malloc
	template<typename T>
	INLINE T* m(const uint16_t size) {
		return reinterpret_cast<T*>(malloc(sizeof(T) * size));
	}
	// Typed Realloc
	template<typename T>
	INLINE T* r(T* ptr, const uint16_t size) {
		return reinterpret_cast<T*>(realloc(ptr, sizeof(T) * size));
	}
	// Free for the sake of completeness
	INLINE void f(void* ptr) {
		return free(ptr);
	}
}

// State of the Display
enum class Display : uint8_t {
	Setup,
	Menu,
	ID
};
#ifdef EXT_SCROLL
// State of Scroller
enum Scroll : bool {
	PAUSE = false, RUN = true
};
#endif // EXT_SCROLL

template<typename T>
struct State {
	using Type = T;
	inline State& operator=(const Type state) {
		value = state;
		return *this;
	}
	constexpr State(const Type state) : value(state) {}
	inline operator Type() const { return value; }
protected:
	Type value;
};
template<>
State<Display>& State<Display>::operator=(const Type state);
#ifdef EXT_SCROLL
template<>
State<Scroll>& State<Scroll>::operator=(const Type state);
#endif // EXT_SCROLL

#ifdef EXT_RECENT
struct History {
	struct Transaction {
		uint8_t index;
		uint8_t value;
	};
	uint16_t count;
	Transaction* queue;

	History() : count(1), queue(alloc::m<Transaction>(1)) {
		queue[0] = Transaction{
			csize::channel,
			0
		};
	}
	void append(const Transaction transaction) {
		cull();
		uint8_t usage = 0;
		for (uint16_t i = 0; i < count; ++i) {
			if (queue[i].index == transaction.index && ++usage >= csize::history) {
				memmove(queue + 1, queue, sizeof(Transaction) * i);
				queue[0] = transaction;
				return;
			}
		}
		auto queue_tmp = alloc::r(queue, ++count);
		// If realloc can not find a new memory block
		// then cycle the queue forwards and retain the current size
		if (!queue_tmp) {
			--count;
		}
		else {
			queue = queue_tmp;
		}
		memmove(queue + 1, queue, sizeof(Transaction) * (count - 1));
		queue[0] = transaction;
	}
	void pop() {
		queue = alloc::r(queue, --count);
	}
	uint16_t first(const uint8_t index) const {
		for (uint16_t i = 0; i < count; ++i) {
			if (queue[i].index == index)
				return queue[i].value;
		}
		return UINT16_MAX;
	}
	uint8_t avg(const uint8_t index) const {
		uint16_t total = 0;
		uint8_t found = 0;
		for (uint16_t i = 0; i < count; ++i) {
			if (queue[i].index == index) {
				total += queue[i].value;
				++found;
			}
		}
		return round(total / static_cast<float>(found));
	}

	void cull() {
		while (free_memory() < MEMORY_CULLING)
			pop();
	}
} history;
#endif // EXT_RECENT

#ifdef EXT_EEPROM
class EEProm {
	struct Chl {
		uint8_t index;

		const bool available() const {
			return EEPROM.read(pos() + offset::header) == magic;
		}
		const uint8_t vmin() const {
			return EEPROM.read(pos() + offset::min);
		}
		const uint8_t vmax() const {
			return EEPROM.read(pos() + offset::max);
		}
		const uint8_t desc(char* buffer) const {
			uint16_t idx = pos() + offset::desc;
			for (uint8_t i = 0; i < size::desc; ++i) {
				buffer[i] = EEPROM.read(idx + i);
			}
			buffer[size::desc] = '\0';
			return strlen(buffer);
		}
	protected:
		constexpr uint16_t pos() const {
			return EEProm::offset::header + static_cast<uint16_t>(index) * size::all;
		}

		// Size of a channel in the EEPROM
		// Header + Min + Max + Desc
		struct size {
			static constexpr uint8_t header = 1;
			static constexpr uint8_t min = 1;
			static constexpr uint8_t max = 1;
			static constexpr uint8_t desc = 15;
			static constexpr uint8_t all = header + min + max + desc;
		};
		struct offset {
			static constexpr uint8_t header = 0;
			static constexpr uint8_t min = header + size::header;
			static constexpr uint8_t max = min + size::min;
			static constexpr uint8_t desc = max + size::max;
		};
	};
	friend Chl;
protected:
	static constexpr uint8_t magic = (EEPROM_MAX - csize::channel);
	struct offset {
		// Size of the header at the start of the eeprom
		// to check the values have been written by this program
		static constexpr uint8_t header = 2;
	};
};

#endif // EXT_EEPROM

struct Channel {
#ifdef EXT_RECENT
	uint8_t avg() const {
		return value.avg();
	}
	struct Value {
		Value(const uint8_t _) {}

		uint8_t avg() const {
			return history.avg(index());
		}
		operator uint16_t() const {
			return history.first(index());
		}

		Value& operator=(const uint8_t rhs) {
			history.append(History::Transaction{
				index(),
				rhs
				});
			return *this;
		}
	protected:
		INLINE uint8_t index() const { return reinterpret_cast<const Channel*>(this)->index(); }

	}
#else // !EXT_RECENT
	uint8_t
#endif // EXT_RECENT
		value{ 0 };

	uint8_t min = 0, max = UINT8_MAX;
	char* desc = nullptr;
	INLINE uint8_t index() const;
};
Channel channels[csize::channel];

INLINE uint8_t Channel::index() const { return this - channels; }


namespace Arrow {
	PICTURE(UP, 0, (B00000, B00100, B01110, B10101, B00100, B00100, B00100, B00000));
	PICTURE(DOWN, 1, (B00000, B00100, B00100, B00100, B10101, B01110, B00100, B00000));

	inline void upload() {
		UP.upload();
		DOWN.upload();
	}
}

struct Timer {
	Timer(const uint16_t interval) : time(millis()), interval(interval) {}
	bool active() {
		if (millis() > time + interval) {
			time += interval;
			return true;
		} return false;
	}
	void reset() {
		time = millis();
	}
	bool reactive() {
		if (active())
			return true;
		reset();
		return false;
	}

protected:
	decltype(millis()) time;
	uint16_t interval;
};

State<Display> state = Display::Setup;
void setup() {
	Serial.begin(BAUD_RATE);
	lcd.begin(WIDTH, HEIGHT);

	// Synchronisation Phase
	lcd.setBacklight(Background::PURPLE);

	Timer timer{ 1000 };
	do {
		if (timer.active())
			Serial.write('Q');
	} while (!Serial.available() || Serial.read() != 'X');

	// Synchronisation Done
	Serial.println(F(EXTENSIONS));
	lcd.setBacklight(Background::WHITE);

	Arrow::upload();
	state = Display::Menu;
}



namespace Window {

	namespace clear {
		static inline void current() {
			lcd.print(F("                ")); // String length of LCD Width
		}
		static inline void line(const uint8_t row) {
			lcd.setCursor(0, row);
			current();
		}
		static inline void all() {
			lcd.clear();
		}
	}

	struct Menu {

		struct Event {
		protected:
			const uint8_t flags;
		public:
			enum Flag : decltype(flags) {
				None = 0b0,
					Head = 0b1,
					Value = 0b10,
					Description = 0b100,
					Index = 0b1000,
					All = Head | Value | Description | Index
			};

			constexpr operator uint8_t() const { return flags; }
			constexpr operator Flag() const { return static_cast<Flag>(flags); }
			constexpr Event(const decltype(flags) flags) : flags(flags) {}
			constexpr inline bool any() const { return flags; }
			constexpr inline bool all() const { return flags == Flag::All; }
			constexpr inline bool head() const { return bitRead(flags, 0); }
			constexpr inline bool value() const { return bitRead(flags, 1); }
			constexpr inline bool description() const { return bitRead(flags, 2); }
			constexpr inline bool indicies() const { return bitRead(flags, 3); }
		};

		void begin() {
			// lcd.setBacklight(Background::WHITE);
			dispatch(Event::Flag::All);
			selector.reset();
		}
		void poll_input() {
			const auto events = lcd.readButtons();

			// Change to ID Mode
			if (events & BUTTON_SELECT) {
				if (selector.active()) {
					state = Display::ID;
					return;
				}
			}
			else {
				selector.reset();
			}

			// Scroll Up
			if (events & BUTTON_UP) {
				const auto next = get_above(index[0]);
				if (next != index[0] && next < csize::channel) {
					index[0] = next;
					return dispatch(Window::Menu::Event::Flag::Index);
				}
			}
			// Scroll Down
			else if (events & BUTTON_DOWN) {
				const auto next = get_below(index[1]);
				if (next != index[1] && next < csize::channel) {
					index[0] = index[1];
					return dispatch(Window::Menu::Event::Flag::Index);
				}
			}

			// TODO: If Right
			// TODO: If Left

#ifdef EXT_SCROLL
// Scrolling
			if (scroller.reactive()) {
				if (scroll == Scroll::PAUSE) {
					scroll = Scroll::RUN;
				}
				++scroll_pos;
			}
#endif // EXT_SCROLL
		}

		using Predicate = bool (*)(uint8_t);

		static uint8_t get_above(uint8_t idx, Predicate predicate) {
			for (uint8_t i = idx - 1; i < UINT8_MAX; --i) {
				if (channels[i].desc && predicate(i))
					return i;
			}
			return UINT8_MAX;
		}
		static uint8_t get_above(uint8_t idx) {
			return get_above(idx, [](uint8_t) -> bool { return true; });
		}

		static uint8_t get_below(uint8_t idx, Predicate predicate) {
			for (uint8_t i = idx + 1; i < csize::channel; ++i) {
				if (channels[i].desc && predicate(i))
					return i;
			}
			return csize::channel;
		}
		static uint8_t get_below(uint8_t idx) {
			return get_below(idx, [](uint8_t) -> bool { return true; });
		}


		bool compute_indices(Predicate predicate) {
			uint8_t old[2] = { index[0], index[1] };
			if (index[0] >= csize::channel)
				index[0] = get_below(UINT8_MAX, predicate);
			index[1] = index[0] != csize::channel ? get_below(index[0], predicate) : csize::channel;
			return (old[0] != index[0]) || (old[1] != index[1]);
		}
		bool compute_indices() {
			return compute_indices([](uint8_t) -> bool { return true; });
		}

		void dispatch(const Event event) {
			if (!event.any())
				return;
			if (event.indicies()) {
#ifdef EXT_SCROLL
				if (compute_indices() || event.all()) {
					dispatch(Event::Flag::All ^ Event::Flag::Index);
					scroll = Scroll::PAUSE;
				}
				else {
					dispatch(Event::Flag::Head);
				}
#else // !EXT_SCROLL
				dispatch(compute_indices() || event.all() ? Event::Flag::All ^ Event::Flag::Index : Event::Flag::Head);
#endif // EXT_SCROLL
				return;
			}

			for (uint8_t backlight = 0, row = 0; row < HEIGHT; ++row) {
				const auto idx = index[row];
				// If first index is invalid so is the second
				if (idx >= csize::channel) return;
				const auto& channel = channels[idx];

				if (event.head()) {
					lcd.setCursor(0, row);
					render_head(row, idx);
				}
				if (event.value()) {
					lcd.setCursor(2, row);
					backlight |= render_value(channel);
					if (row || (index[1] >= csize::channel))
						lcd.setBacklight(backlight ? backlight : Background::WHITE);
				}
				if (event.description()) {
					lcd.setCursor(csize::start_pos, row);
					clear::current();
					lcd.setCursor(csize::start_pos, row);
					render_description(channel);
				}
			}
		}

		void render_head(const uint8_t row, const uint8_t idx) const {
			lcd.write((row ? get_below(idx) : get_above(idx)) < csize::channel ? (row ? Arrow::DOWN : Arrow::UP) : ' ');
			lcd.write(idx + 'A');
		}

		void render_single(const uint8_t value) const {
			const uint8_t logged = 2 - static_cast<uint8_t>(log10(value));
			for (uint8_t i = 0; i < logged; ++i)
				lcd.write(' ');
			lcd.print(value);
		}
		Background render_value(const Channel& channel) const {
			const uint16_t value = channel.value; // Cache the value;
			if (value > UINT8_MAX) {
#ifdef EXT_RECENT
				lcd.print(F("       "));
#else // !EXT_RECENT
				lcd.print(F("   "));
#endif // EXT_RECENT
				return Background::CLEAR;
			}
			render_single(channel.value);
#ifdef EXT_RECENT
			lcd.write(',');
			render_single(channel.avg());
#endif // EXT_RECENT
			return (channel.value > channel.max) ? Background::RED : (channel.value < channel.min ? Background::GREEN : Background::CLEAR);
		}

		void render_description(const Channel& channel) const {
			lcd.write(' ');
#ifdef EXT_SCROLL
			if (scroll_pos <= strlen(channel.desc))
				lcd.print(channel.desc + scroll_pos);
			clear::current();
#else // !EXT_SCROLL
			lcd.print(channel.desc);
#endif // EXT_SCROLL
		}
	protected:
		uint8_t index[2] = { UINT8_MAX, csize::channel };
		Timer selector{ 1000 };
#ifdef EXT_SCROLL
		friend State<Scroll>;
		State<Scroll> scroll{ Scroll::PAUSE };
		uint8_t scroll_pos{ 0 };
		Timer scroller{ 1500 }; // 1.5sec delay
#endif // EXT_SCROLL
	} menu;

	struct ID {
		void begin() {
			lcd.setBacklight(Background::PURPLE);
			ram = RAM_MAX + 1;
			render();
		}
		void poll_input() {
			if (!(lcd.readButtons() & BUTTON_SELECT)) {
				state = Display::Menu;
				return;
			}
			render();
		}
		void render() {
			const decltype(ram) current = free_memory();
			if (current != ram) {
				lcd.setCursor(0, 0);
				lcd.print(F(STUDENT_ID));
				lcd.setCursor(0, 1);
				lcd.print(current);
				lcd.write('B');
			}
		}
	protected:
		uint16_t ram;
	} id;
} // namespace Window

namespace Protocol {
	inline void exhaust_serial() {
		if (Serial.available())
			while (Serial.read() != '\n');
	}

	inline Channel* read_data(char* buffer, const uint8_t size) {
		const uint8_t len = Serial.readBytesUntil('\n', buffer, size);
		const uint8_t index = (*buffer) - 'A';
		if ((index) > csize::channel) {
			return nullptr;
		}
		buffer[0] = len - 1;
		return &channels[index];
	}

	inline bool create(char* buf, uint8_t& event) {
		auto channel = read_data(buf, csize::create);
		if (!channel) return false;
		if (channel->desc) {
			channel->desc = alloc::r(channel->desc, buf[0] + 1);
		}
		else {
			channel->desc = alloc::m<char>(buf[0] + 1);
		}
		memcpy(channel->desc, buf + 1, buf[0]);
		channel->desc[buf[0]] = '\0';
		event |= Window::Menu::Event::Flag::Index;
		history.cull();
		return true;
	}

	inline uint16_t convert_int(char* buf) {
		uint16_t total = 0;
		for (uint8_t i = buf[0], p = 1; i > 0 && '0' <= buf[i] && buf[i] <= '9'; --i, p *= 10) {
			total += (buf[i] - '0') * p;
		}
		return total;
	}

	enum OP {
		CREATE = 'C',
		VALUE = 'V',
		MIN = 'N',
		MAX = 'X',
		NOOP = '\n'
	};

	bool write(const OP op, char* buf, uint8_t& event) {
		auto channel = read_data(buf, csize::write);
		if (!channel) return false;
		if (!channel->desc || !buf[0]) {
			buf[0] = channel->index() + 'A';
			return false;
		}
		const auto v = convert_int(buf);
		if (v > UINT8_MAX)	return false;
		switch (op) {
			case OP::VALUE:
				channel->value = v;
				break;
			case OP::MIN:
				channel->min = v;	break;
			case OP::MAX:
				channel->max = v;	break;
		}
		event |= Window::Menu::Event::Flag::Value;
		return true;
	}

	inline Window::Menu::Event process() {
		uint8_t event = Window::Menu::Event::Flag::None;
		if (Serial.available()) {
			bool result = false;
			char buf[csize::protocol] = { '\0' };
			const char v = Serial.read();
			switch (v) {
				case OP::NOOP:	return event;
				case OP::CREATE:	result = create(buf, event); break;
				case OP::VALUE:	result = write(OP::VALUE, buf, event); break;
				case OP::MAX:	result = write(OP::MAX, buf, event); break;
				case OP::MIN:	result = write(OP::MIN, buf, event); break;
				default:
					Serial.readBytesUntil('\n', buf, csize::protocol);
			}
			if (!result) {
				Serial.print(F("ERROR: "));
				Serial.print(v);
				Serial.println(buf);
			}
			else {
				log_debug(buf);
			}
			exhaust_serial();
		}
		return event;
	}
} // namespace Protocol

template<>
State<Display>& State<Display>::operator=(const Type state) {
	if (state == value)	return *this;
	value = state;
	Window::clear::all();
	switch (value) {
		case Type::Menu:
			Window::menu.begin(); break;
		case Type::ID:
			Window::id.begin(); break;
	}	return *this;
}
#ifdef EXT_SCROLL
template<>
State<Scroll>& State<Scroll>::operator=(const Type state) {
	if (state == value)	return *this;
	value = state;
	switch (value) {
		case Type::PAUSE:
			Window::menu.scroller = Timer{ 1500 }; //2 second delay
			break;
		case Type::RUN:
			Window::menu.scroller = Timer{ 500 }; // 2 char/sec
			break;
	}
	Window::menu.scroll_pos = 0;
	return *this;
}
#endif // EXT_SCROLL

void loop() {
	const auto event = Protocol::process();
	switch (state) {
		case Display::Menu:
			Window::menu.poll_input();
			return Window::menu.dispatch(event);
		case Display::ID:
			return Window::id.poll_input();
	}
}
