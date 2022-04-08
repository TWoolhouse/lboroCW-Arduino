#include <Wire.h>
#include <Adafruit_RGBLCDShield.h>
#include <utility/Adafruit_MCP23017.h>
#include <EEPROM.h>

Adafruit_RGBLCDShield lcd;

// #define DEBUG
#define STUDENT_ID "F121584"

// Constants
namespace cexpr {
	constexpr uint16_t baud_rate = 9600;
	constexpr uint8_t lcd_width = 16;
	constexpr uint8_t lcd_height = 2;

	// Maximum Potential RAM of the Board
	constexpr uint16_t ram_size = RAMEND - RAMSTART;
	constexpr uint16_t eeprom_size = E2END + 1;
	// Amount of Free Ram left to begin culling dynamic memory
	// Such that the stack does not become corrupted
	constexpr uint16_t memory_cull = 512;

	constexpr uint8_t desc = 16; // Includes null
	constexpr uint8_t create = desc - 1;
	constexpr uint8_t write = 3;
	constexpr uint8_t protocol = max(desc, write) + 1;
	constexpr uint8_t channels = 26;
	constexpr uint8_t history = 64;
}
#define EXTENSIONS "UDCHARS,FREERAM,HCI,EEPROM,RECENT,NAMES,SCROLL"

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

// Log Double Debug - A pair of values
#define log_ddebug(message, value) { \
Serial.print(F("DEBUG: ")); \
Serial.print(millis()); \
Serial.print(' '); \
Serial.print(__PRETTY_FUNCTION__); \
Serial.print(' '); \
Serial.print(__LINE__); \
Serial.print(F(" >> ")); \
Serial.print(message); \
Serial.print(F(": ")); \
Serial.println(value); \
}
#else // !DEBUG
#define log_debug(message)
#define log_ddebug(message, value)
#endif // DEBUG

// Helper Macros
#define BRACED_INIT_LIST(...) { __VA_ARGS__ }
#define sizeof_arr(arr, type) (sizeof(arr) / sizeof(type))

#if defined(_MSC_VER) // Force Inline
#define INLINE inline __forceinline
#elif defined(__GNUC__) || defined(__GNUG__) || defined(__clang__)
#define INLINE inline __ATTR_GNU_INLINE__
#else
#define INLINE inline
#endif // Force Inline

// Custom Character Pictures
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

#pragma region Free Memory
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
#pragma endregion Free Memory

// Memory Allocators
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

// LCD Backlight Controller
struct Backlight {
	enum Colour : uint8_t {
		CLEAR = 0,
		RED = 1,
		GREEN = 2,
		YELLOW = 3,
		PURPLE = 5,
		WHITE = 7
	};
	Colour colour;
	Backlight() : colour(Colour::CLEAR) {}
	inline Backlight& operator=(const Colour colour) {
		this->colour = colour;
		lcd.setBacklight(colour);
		return *this;
	}
} Backlight;

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
protected:
	decltype(millis()) time;
	uint16_t interval;
};

// State of the Display
enum class Display : uint8_t {
	Setup,
	Menu,
	ID
};

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

enum class Scroll : uint8_t {
	PAUSE, RUN
};

class Scroller : public State<Scroll> {
public:
	uint8_t pos;
	Timer timer;

	Scroller() : State(Scroll::PAUSE), pos(0), timer(2000) {}

	Scroller& operator=(const Type state) {
		timer.reset();
		if (state == value)	return *this;
		value = state;
		switch (value) {
			case Type::PAUSE:
				timer = Timer{ 2000 }; //2 second delay
				break;
			case Type::RUN:
				timer = Timer{ 500 }; // 2 char/sec
				break;
		}
		pos = 0;
		return *this;
	}
};

namespace Arrow {
	PICTURE(UP, 0, (B00000, B00100, B01110, B10101, B00100, B00100, B00100, B00000));
	PICTURE(DOWN, 1, (B00000, B00100, B00100, B00100, B10101, B01110, B00100, B00000));

	inline void upload() {
		UP.upload();
		DOWN.upload();
	}
}

State<Display> state(Display::Setup);

struct History {
	struct Transaction {
		uint8_t index;
		uint8_t value;
	};
	uint16_t count;
	Transaction* queue;

	History() : count(1), queue(alloc::m<Transaction>(1)) {
		queue[0] = Transaction{
			cexpr::channels,
			0
		};
	}
	void append(const Transaction transaction) {
		cull();
		uint8_t usage = 0;
		for (uint16_t i = 0; i < count; ++i) {
			if (queue[i].index == transaction.index && ++usage >= cexpr::history) {
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
		while (free_memory() < cexpr::memory_cull)
			pop();
	}
} history;

struct Event {
protected:
	uint8_t flags;
public:
	enum Flag : decltype(flags) {
		None = 0b0,
			Head = 0b1,
			Value = 0b10,
			Description = 0b100,
			All = Head | Value | Description
	};

	constexpr operator Flag() const { return static_cast<Flag>(flags); }
	constexpr Event(const decltype(flags) flags) : flags(flags) {}
	constexpr inline bool any() const { return flags; }
	constexpr inline bool all() const { return flags == Flag::All; }
	constexpr inline bool head() const { return bitRead(flags, 0); }
	constexpr inline bool value() const { return bitRead(flags, 1); }
	constexpr inline bool description() const { return bitRead(flags, 2); }
};

namespace Channel {
	class eeprom {
	public:
		static void setup() {
			bool invalid = false;
			for (uint8_t i = 0; i < size::precheck; ++i) {
				const uint8_t idx = offset::precheck + i;
				if (EEPROM.read(idx) != magic) {
					invalid = true;
					EEPROM.update(idx, magic);
				}
			}
			if (invalid) {
				for (uint8_t i = 0; i < cexpr::channels; ++i) {
					if (EEPROM.read(pos(i) + offset::header) == magic)
						EEPROM.update(pos(i) + offset::header, magic + 1);
				}
			}
		}

		static const bool available(const uint8_t index) {
			return EEPROM.read(pos(index) + offset::header) == magic;
		}
		static const uint8_t boundary(const uint8_t index, const uint8_t which) {
			return EEPROM.read(pos(index) + which);
		}
		static const char* desc(const uint8_t index) {
			static char cache[cexpr::desc];
			uint16_t idx = pos(index) + offset::desc;
			for (uint8_t i = 0; i < size::desc; ++i) {
				cache[i] = EEPROM.read(idx + i);
			}
			cache[size::desc] = '\0';
			return cache;
		}

		static void create(const uint8_t index, const uint8_t min, const uint8_t max) {
			EEPROM.update(pos(index) + offset::header, magic);
			boundary(index, offset::min, min);
			boundary(index, offset::max, max);
		}

		static void boundary(const uint8_t index, const uint8_t which, uint8_t val) {
			EEPROM.update(pos(index) + which, val);
		}
		static void desc(uint8_t index, const char* buffer) {
			const uint16_t idx = pos(index) + offset::desc;
			const uint8_t length = min(strlen(buffer), size::desc);
			for (uint8_t i = 0; i <= length; ++i) {
				EEPROM.update(idx + i, buffer[i]);
			}
		}

	protected:
		INLINE static constexpr uint16_t pos(const uint8_t index) {
			return offset::precheck + size::precheck + static_cast<uint16_t>(index) * size::all;
		}

		static constexpr uint8_t magic = (cexpr::eeprom_size - cexpr::channels);
		// Size of a channel in the EEPROM
		// Header + Min + Max + Desc
		struct size {
			static constexpr uint8_t precheck = 2;
			static constexpr uint8_t header = 1;
			static constexpr uint8_t min = 1;
			static constexpr uint8_t max = 1;
			static constexpr uint8_t desc = cexpr::desc - 1;
			static constexpr uint8_t all = header + min + max + desc;
		};
	public:
		struct offset {
			static constexpr uint8_t precheck = 0;
			static constexpr uint8_t header = 0;
			static constexpr uint8_t min = header + size::header;
			static constexpr uint8_t max = min + size::min;
			static constexpr uint8_t desc = max + size::max;
		};
	};

	union View {
	private:
		struct AttrHelper { INLINE const uint8_t index() const { return reinterpret_cast<const View*>(this)->index; } };
		struct Value : protected AttrHelper {
			inline operator const uint16_t() const { return history.first(index()); }
			inline Value& operator=(const uint8_t rhs) { history.append({ index(), rhs }); return *this; }
			const uint8_t avg() const { return history.avg(index()); }
		};
		struct Description : protected AttrHelper {
			inline operator const char* () const { return eeprom::desc(index()); }
			inline Description& operator=(const char* rhs) { eeprom::desc(index(), rhs); return *this; }
		};
		template<uint8_t Pos>
		struct Boundary : protected AttrHelper {
			inline operator const uint8_t() const { return eeprom::boundary(index(), Pos); }
			inline Boundary& operator=(const uint8_t rhs) { eeprom::boundary(index(), Pos, rhs); return *this; }
		};
	public:
		constexpr View(const uint8_t channel) : index(channel) {}
		uint8_t index;
		Value value;
		Description desc;
		Boundary<eeprom::offset::min> min;
		Boundary<eeprom::offset::max> max;

		INLINE const char letter() const { return index + 'A'; }

		INLINE bool valid() const { return index < cexpr::channels; }

		bool exists() const {
			if (!valid())
				return false;
			return eeprom::available(index);
		}

		inline operator const uint8_t() const { return index; }

		const Backlight::Colour backlight() const {
			const uint16_t val = value;
			if (valid() && val <= UINT8_MAX)
				return (val > max ? Backlight::Colour::RED : (val < min ? Backlight::Colour::GREEN : Backlight::Colour::CLEAR));
			return Backlight::Colour::CLEAR;
		}

		void log() const {
			log_ddebug(letter(), desc);
			log_ddebug(F("Value"), value);
			log_ddebug(F("Min"), min);
			log_ddebug(F("Max"), max);
		}

	};

	struct Display {
		uint8_t row;
		View channel;
		Event events;
		Scroller scroll;

		static Display* active(const View channel);

		Display() : row(2), channel(cexpr::channels), events(Event::Flag::All), scroll() {}
		Display(uint8_t row, View channel) : row(row), channel(channel), events(Event::Flag::All), scroll() {}

		Display& operator=(View channel) {
			if (this->channel.index == channel.index)	return *this;
			if (!active(channel))
				scroll = Scroll::PAUSE;
			this->channel = channel;
			events = Event::Flag::All;
			return *this;
		}
		Display& operator=(const Display& other) {
			if (this == &other)	return *this;
			channel = other.channel;
			memcpy(&scroll, &other.scroll, sizeof(Scroller));
			events = Event::Flag::All;
			return *this;
		}

		INLINE bool valid() { return channel.exists(); }
		inline void event(const Event::Flag event) { events = events | event; }
		inline void reset() { events = Event::Flag::None; }
	};
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

	struct Render {
		static void head(Channel::Display& display, const uint8_t arrow) {
			lcd.setCursor(0, display.row);
			lcd.write(arrow);
			lcd.write(display.channel.letter());
		}
		static void value(Channel::Display& display) {
			lcd.setCursor(2, display.row);
			const uint16_t val = display.channel.value;
			if (val > UINT8_MAX) {
				lcd.print(F("       "));
			}
			else {
				single_value(val);
				lcd.write(',');
				single_value(display.channel.value.avg());
			}

		}
		static void description(Channel::Display& display) {
			// TODO: Scroll
			lcd.setCursor(10, display.row);
			const uint8_t len = strlen(display.channel.desc);
			if (len > cexpr::lcd_width - 10)
				lcd.print(display.channel.desc + display.scroll.pos % (len + 1));
			else
				lcd.print(display.channel.desc);
			clear::current();
		}
		static void layout(Channel::Display& display) {
			lcd.setCursor(9, display.row);
			lcd.write(' ');
		}
	protected:
		static void single_value(const uint8_t val) {
			const uint8_t spaces = 2 - static_cast<uint8_t>(log10(val));
			for (uint8_t i = 0; i < spaces; ++i) {
				lcd.write(' ');
			}
			lcd.print(val);
		}
	};

	class Menu {
	public:
		enum Direction : int8_t {
			UP = -1,
			CONSTANT = 0,
			DOWN = 1,
			PREDICATE = 2,
		};
	protected:
		friend Channel::Display;
		Timer selector{ 1000 };
		Channel::Display channels[cexpr::lcd_height] = { {0, UINT8_MAX}, {1, UINT8_MAX} };
		uint8_t last_input = 0;

		struct Predicate {
			using Func = bool(*)(const Channel::View channel);
			static constexpr bool all(const Channel::View) { return true; }
			static bool maximum(const Channel::View channel) { return channel.value <= UINT8_MAX && channel.value > channel.max; } // TODO: Implement predicate
			static bool minimum(const Channel::View channel) { return channel.value <= UINT8_MAX && channel.value < channel.min; } // TODO: Implement predicate
		};
		struct PredicateState : public State<Predicate::Func> {
			using State::State; // Inherit Constructor
			PredicateState& operator=(const Type state);
		} predicate{ Predicate::all };

		const uint8_t find_up(uint8_t idx) {
			for (--idx; idx < UINT8_MAX && !(Channel::View(idx).exists() && predicate(idx)); --idx);
			if (Channel::View(idx).exists())
				return idx;
			return UINT8_MAX;
		}
		const uint8_t find_down(uint8_t idx) {
			for (++idx; idx < cexpr::channels && !(Channel::View(idx).exists() && predicate(idx)); ++idx);
			if (Channel::View(idx).exists())
				return idx;
			return cexpr::channels;
		}

		bool render(Channel::Display& display) {
			if (!display.events.any()) return;
			if (!display.valid()) {
				clear::line(display.row);
				return false;
			}

			if (display.events.head()) {
				Render::head(display, ((*this).*(display.row ? &find_down : &find_up))(display.channel.index) < cexpr::channels ? (display.row ? Arrow::DOWN : Arrow::UP) : ' ');
			}
			if (display.events.value())
				Render::value(display);
			if (display.events.description())
				Render::description(display);
			if (display.events.all())
				Render::layout(display);

			return display.events.value();
		}

		bool evaluate_index(const Direction dir, Channel::Display(&previous)[cexpr::lcd_height]) {
			switch (dir) {
				case Direction::UP:
					if (const auto next = find_up(channels[0].channel); Channel::View(next).valid()) {
						channels[0] = next;
						channels[1] = previous[0];
						return true;
					}	return false;

				case Direction::CONSTANT:
					channels[1] = find_down(channels[0].channel);
					return channels[1].channel != previous[1].channel;

				case Direction::DOWN:
					if (const auto next = find_down(channels[1].channel); Channel::View(next).valid()) {
						channels[0] = previous[1];
						channels[1] = next;
						return true;
					}	return false;

				case Direction::PREDICATE:
					if (!evaluate_index(Direction::UP, previous))
						channels[0] = find_down(UINT8_MAX);
					evaluate_index(Direction::CONSTANT, previous);
					return true;
			}
			return false;
		}

	public:
		void begin() {
			Backlight = Backlight::Colour::WHITE;
			for (auto& display : channels)
				display.scroll = Scroll::PAUSE;
			event(Event::Flag::All);
			render();
			selector.reset();
		}
		void render() {
			bool backlight = false;
			for (auto& display : channels) {
				backlight = render(display);
				display.reset();
			}
			if (backlight) {
				Backlight::Colour bl = channels[0].channel.backlight() | channels[1].channel.backlight();
				Backlight = bl == Backlight::Colour::CLEAR ? Backlight::Colour::WHITE : bl;
			}
		}
		bool poll_input() {
			uint8_t last_input = this->last_input;
			const auto events = lcd.readButtons();
			this->last_input = events;

			// Change to ID Mode
			if (events & BUTTON_SELECT) {
				if (selector.active()) {
					state = Display::ID;
					return false;
				}
			}
			else {
				selector.reset();
			}

			for (auto& display : channels) {
				if (display.scroll.timer.active()) {
					if (display.scroll == Scroll::PAUSE)
						display.scroll = Scroll::RUN;
					++display.scroll.pos;
					event(Event::Flag::Description);
					display.scroll.timer.reset();
				}
			}

			if (events & BUTTON_UP && evaluate_index(Direction::UP))
				return true;
			if (events & BUTTON_DOWN && evaluate_index(Direction::DOWN))
				return true;

			if (last_input & BUTTON_LEFT && !(events & BUTTON_LEFT)) {
				if (predicate == Predicate::all)
					predicate = Predicate::minimum;
				else if (predicate == Predicate::minimum)
					predicate = Predicate::all;
			}
			if (last_input & BUTTON_RIGHT && !(events & BUTTON_RIGHT)) {
				if (predicate == Predicate::all)
					predicate = Predicate::maximum;
				else if (predicate == Predicate::maximum)
					predicate = Predicate::all;
			}
			return true;
		}
		inline void event(const Event::Flag event) {
			for (auto& display : channels)
				display.event(event);
		}
		void headers() {
			event(Event::Flag::Head);
			evaluate_index(Window::Menu::Direction::CONSTANT);
		}
		bool evaluate_index(const Direction dir) {
			if (channels[0].channel >= cexpr::channels)
				if (channels[0] = find_down(UINT8_MAX); !channels[0].valid())
					return false;
			Channel::Display old[cexpr::lcd_height];
			memcpy(&old, &channels, sizeof(old));
			return evaluate_index(dir, old);
		}
	} menu;
	Menu::PredicateState& Menu::PredicateState::operator=(const Type state) {
		if (state == value)	return *this;
		value = state;
		menu.evaluate_index(value == Predicate::all ? Direction::CONSTANT : Direction::PREDICATE);
		menu.event(Event::Flag::Head);
	}

	struct ID {
		void begin() {
			Backlight = Backlight::Colour::PURPLE;
			ram = cexpr::ram_size + 1;
			render();
		}
		void poll_input() {
			if (!(lcd.readButtons() & BUTTON_SELECT))
				state = Display::Menu;
		}
		void render() {
			const decltype(ram) current = free_memory();
			if (current != ram) {
				ram = current;
				lcd.setCursor(0, 0);
				lcd.print(F(STUDENT_ID));
				lcd.setCursor(0, 1);
				lcd.print(ram);
				lcd.write('B');
			}
		}
	protected:
		uint16_t ram;
	} id;
}

namespace Channel {
	Display* Display::active(const View channel) {
		for (auto& display : Window::menu.channels) {
			if (display.channel.index == channel.index)
				return &display;
		}
		return nullptr;
	}
}

namespace Protocol {
	inline void exhaust_serial() {
		if (Serial.available())
			while (Serial.read() != '\n');
	}

	inline Channel::View read_data(char* buffer, const uint8_t size) {
		const uint8_t len = Serial.readBytesUntil('\n', buffer, size + 1); // Plus 1 for the channel
		const uint8_t idx = *buffer - 'A';
		buffer[0] = len - 1;
		return Channel::View(idx);
	}

	inline bool create(char* buf) {
		auto channel = read_data(buf, cexpr::create);
		if (!channel.valid()) {
			buf[0] = channel.letter();
			return false;
		}
		buf[buf[0] + 1] = '\0';
		if (!channel.exists())
			Channel::eeprom::create(channel, 0, UINT8_MAX);
		channel.desc = buf + 1;
		Window::menu.headers();
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

	bool write(const OP op, char* buf) {
		auto channel = read_data(buf, cexpr::write);
		if (!channel.exists()) return false;
		if (!channel.desc || !buf[0]) {
			buf[0] = channel.letter();
			return false;
		}
		const auto v = convert_int(buf);
		if (v > UINT8_MAX)	return false;
		switch (op) {
			case OP::VALUE:
				channel.value = v;	break;
			case OP::MIN:
				channel.min = v;	break;
			case OP::MAX:
				channel.max = v;	break;
		}
		if (auto display = Channel::Display::active(channel))
			display->event(Event::Flag::Value);

		Window::menu.evaluate_index(Window::Menu::Direction::CONSTANT);
		channel.log();
		return true;
	}

	inline void process() {
		if (Serial.available()) {
			bool result = false;
			char buf[cexpr::protocol]{ '\0' };
			const char v = Serial.read();
			switch (v) {
				case OP::NOOP:		return;
				case OP::CREATE:	result = create(buf); break;
				case OP::VALUE:		result = write(OP::VALUE, buf); break;
				case OP::MAX:		result = write(OP::MAX, buf); break;
				case OP::MIN:		result = write(OP::MIN, buf); break;
				default:
					Serial.readBytesUntil('\n', buf, cexpr::protocol);
			}
			if (!result) {
				Serial.print(F("ERROR: "));
				Serial.print(v);
				Serial.println(buf);
			}
			else {
				buf[0] += '0';
				log_ddebug(v, buf);
			}
			exhaust_serial();
		}
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

void setup() {
	Serial.begin(cexpr::baud_rate);
	lcd.begin(cexpr::lcd_width, cexpr::lcd_height);

	// Synchronisation Phase
	Backlight = Backlight::Colour::PURPLE;

	Timer timer{ 1000 };
	do {
		if (timer.active())
			Serial.write('Q');
	} while (!Serial.available() || Serial.read() != 'X');

	// Synchronisation Done
	Serial.println(F(EXTENSIONS));
	Backlight = Backlight::Colour::WHITE;

	Arrow::upload();
	Channel::eeprom::setup();
	Window::menu.evaluate_index(Window::Menu::Direction::CONSTANT);
	state = Display::Menu;
}

void loop() {
	Protocol::process();
	switch (state) {
		case Display::Menu:
			if (Window::menu.poll_input())
				Window::menu.render();
			return;
		case Display::ID:
			Window::id.poll_input();
			return Window::id.render();
	}
}
