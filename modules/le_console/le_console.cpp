#include "le_core.h"
#include "le_hash_util.h"

#include "le_log.h"
#include "le_console.h"
#include "le_settings.h"

#include <mutex>
#include <sstream>
#include <thread>
#include <deque>

#include <assert.h>
#include <errno.h>
#include <string.h>

#include <memory>
#include <vector>
#include <iostream>
#include <fstream>
#include <unordered_map>

#include "private/le_console/le_console_types.h"
#include "private/le_console/le_console_server.h"

// Translation between winsock and posix sockets
#ifdef _WIN32
#	define strtok_reentrant strtok_s
#else
#	define strtok_reentrant strtok_r
#endif

extern void le_console_server_register_api( void* api ); // in le_console_server

/*

Ideas for this module:

we want to have more than one console - perhaps one console per connection.

Right now we have a situation where we have a console that is always-on,
and subscribes to log messages as soon as its server starts.

Any connections to the console receive the same answer - there is only one channel for
input and one channel for output. All connections feed their commands into the same
channel for commands.

commands get processed when explicitly requested.

*

A console is something that can subscribe to messages from the log for example,
but most importantly it can read commands from a socket and write responses back to that socket.

*/

static void**                  PP_CONSOLE_SINGLETON  = nullptr; // set via registration method
static le_console_server_api_t le_console_server_api = {};

static constexpr auto NO_CONSOLE_MSG = "Could not find console. You must create at least one console object.";

static le_console_o* produce_console() {
	return static_cast<le_console_o*>( *PP_CONSOLE_SINGLETON );
}

// ----------------------------------------------------------------------
// This method may not log via le::log, because otherwise there is a
// chance of a deadlock.
static void logger_callback( char const* chars, uint32_t num_chars, void* user_data ) {
	auto connection = ( le_console_o::connection_t* )user_data;
	connection->channel_out.post( std::string( chars, num_chars ) + "\r\n" );
}

// ----------------------------------------------------------------------

// If we initialize an object from this and store it static,
// then the destructor will get called when this module is being unloaded
// this allows us to remove ourselves from listeners before the listener
// gets destroyed.
class LeLogSubscriber : NoCopy {
  public:
	explicit LeLogSubscriber( le_console_o::connection_t* connection )
	    : handle( le_log::api->add_subscriber( logger_callback, connection, connection->log_level_mask ) ) {
		static auto logger = le::Log( LOG_CHANNEL );
		logger.debug( "Adding Log subscriber with mask 0x%x", connection->log_level_mask );
	}
	~LeLogSubscriber() {
		static auto logger = le::Log( LOG_CHANNEL );
		logger.debug( "Removing Log subscriber" );
		// we must remove the subscriber because it may get called before we have a chance to change the callback address -
		// even callback forwarding won't help, because the reloader will log- and this log event will happen while the
		// current module is not yet loaded, which means there is no valid code to run for the subscriber.
		le_log::api->remove_subscriber( handle );
	};

  private:
	uint64_t handle;
};

// ----------------------------------------------------------------------
// We use this convoluted construction of a unique_ptr around a Raii class,
// so that we can detect when a module is being unloaded - in which case
// the unique_ptr will call the destructor on LeLogSubscriber.
//
// We must un-register the subscriber in case that this module is reloaded,
// because the loader itself might log, and this logging call may trigger
// a call onto an address which has just been unloaded...
//
// Even callback-forwarding won't help, because the call will happen in the
// liminal stage where this module has been unloaded, and its next version
// has not yet been loaded.
//
static std::unordered_map<uint32_t, std::unique_ptr<LeLogSubscriber>>& le_console_produce_log_subscribers() {
	static std::unordered_map<uint32_t, std::unique_ptr<LeLogSubscriber>> LOG_SUBSCRIBER = {};

	return LOG_SUBSCRIBER;
}

// ----------------------------------------------------------------------

le_console_o::connection_t::~connection_t() {
	// Remove subscribers if there were any
	this->wants_log_subscriber = false;
	le_console_produce_log_subscribers()[ this->fd ].reset( nullptr );
}

// ----------------------------------------------------------------------

// We want to start the server on-demand.
// if the module gets unloaded, the server thread needs to be first stopped
// if the module gets reloaded, the server thread needs to be resumed
class ServerWatcher : NoCopy {
  public:
	ServerWatcher( le_console_o* console_ )
	    : console( console_ ) {
		if ( console && console->server ) {
			le_console_server_api.start_thread( console->server );
		}
	};
	~ServerWatcher() {
		if ( console && console->server ) {
			// we must stop server
			le_console_server_api.stop_thread( console->server );
		}
	};

  private:
	le_console_o* console = nullptr;
};

// ----------------------------------------------------------------------

static std::unique_ptr<ServerWatcher>& le_console_produce_server_watcher( le_console_o* console = nullptr ) {
	static std::unique_ptr<ServerWatcher> SERVER_WATCHER = {};

	if ( nullptr == SERVER_WATCHER.get() && console != nullptr ) {
		SERVER_WATCHER = std::make_unique<ServerWatcher>( console );
	}
	return SERVER_WATCHER;
}
// ----------------------------------------------------------------------

static bool le_console_server_start() {
	static auto logger = le::Log( LOG_CHANNEL );

	le_console_o* self = produce_console();

	if ( self == nullptr ) {
		logger.error( NO_CONSOLE_MSG );
		return false;
	}

	logger.info( "* Starting Server..." );

	self->server = le_console_server_api.create( self ); // destroy server
	le_console_server_api.start( self->server );         // setup server
	le_console_produce_server_watcher( self );           // Implicitly starts server thread

	return true;
}

// ----------------------------------------------------------------------

static bool le_console_server_stop() {
	static auto logger = le::Log( LOG_CHANNEL );

	le_console_o* self = produce_console();

	if ( self == nullptr ) {
		logger.error( NO_CONSOLE_MSG );
		return false;
	}

	le_console_produce_log_subscribers().clear();

	if ( self->server ) {
		logger.info( "* Stopping server..." );
		le_console_produce_server_watcher().reset( nullptr ); // explicitly call destructor on server watcher - this will join the server thread
		le_console_server_api.stop( self->server );           // stop server
		le_console_server_api.destroy( self->server );        // destroy server
		self->server = nullptr;
	}

	return true;
}

std::string telnet_get_suboption( std::string::iterator& it, std::string::iterator str_end ) {
	// suboption is anything that is

	constexpr auto NVT_SB = char( 250 );
	constexpr auto NVT_SE = char( 240 );

	while ( it != str_end && *it != NVT_SB ) {
		it++;
	}

	auto sub_start = ++it;

	for ( ;
	      it != str_end && *it != NVT_SE;
	      it++ ) {
	}

	return std::string( sub_start, it );
};

// ----------------------------------------------------------------------

// will update stream_begin to point at one part the last character of the last command
// that was interpreted
std::string telnet_filter( le_console_o::connection_t*       connection,
                           std::string::const_iterator       stream_begin,
                           std::string::const_iterator const stream_end ) {

	static auto logger = le::Log( LOG_CHANNEL );

	if ( connection == nullptr ) {
		assert( false && "Must have valid connection" );
		return "";
	}
	// ----------| invariant: connection is not nullptr

	if ( stream_end - stream_begin <= 0 ) {
		// no characters to process.
		return "";
	}

	std::string result;
	result.reserve( stream_end - stream_begin ); // pre-allocate maximum possible chars that this operation might need

	// Find next command
	// Set iterator `it` to one past next IAC byte. starts search at iterator position `it`
	// return true if a command was found
	static auto find_next_command = []( std::string::const_iterator& it, std::string::const_iterator const& it_end ) -> bool {
		if ( it == it_end ) {
			return false;
		}
		const uint8_t IAC = '\xff';
		while ( it != it_end ) {
			if ( uint8_t( *it ) == IAC ) {

				// if there is a lookahead character, and it is in
				// fact a IAC character, this means that IAC is meant
				// to be escaped and interpreted literally.
				if ( it + 1 != it_end && ( uint8_t( *( it + 1 ) ) == IAC ) ) {
					it = it + 2;
					continue;
				}

				++it;
				return true;
			}
			it++;
		}
		return false;
	};

	enum class TEL_OPT : uint8_t {
		SB   = 0xfa, // suboption begin
		SE   = 0xf0, // suboption end
		WILL = 0xfb,
		WONT = 0xfc,
		DO   = 0xfd,
		DONT = 0xfe,
	};

	// if is_option, returns true and sets it_end to one past the last character that is part of the option
	// otherwise return false and leave it_end untouched.
	static auto is_option = []( std::string::const_iterator const it, std::string::const_iterator& it_end ) -> bool {
		if ( it == it_end ) {
			return false;
		}
		// ---------| invariant: we are not at the end of the stream

		if ( it + 1 == it_end ) {
			return false;
		}
		// ----------| invariant: there is a next byte available

		if ( uint8_t( *it ) >= uint8_t( TEL_OPT::WILL ) &&
		     uint8_t( *it ) <= uint8_t( TEL_OPT::DONT ) ) {
			it_end = it + 2;
			return true;
		}

		return false;
	};

	// if is_option, returns true and sets it_end to one past the last character that is part of the option
	// if is not a sub option, return false, don't touch it_end
	static auto is_sub_option = []( std::string::const_iterator const it, std::string::const_iterator& it_end ) -> bool {
		if ( it == it_end ) {
			return false;
		}
		// ---------| invariant: we are not at the end of the stream

		if ( uint8_t( *it ) != uint8_t( TEL_OPT::SB ) ) { // suboption start
			return false;
		}

		auto sub_option_end = it + 1; // start our search for suboption end command one byte past the current byte

		if ( !find_next_command( sub_option_end, it_end ) ) {
			return false;
		}

		// ---------| invariant: we managed to find the next IAC byte

		if ( sub_option_end == it_end ) {
			return false;
		}

		// ----------| invariant: there is a next byte available
		// look for "suboption end byte"
		if ( uint8_t( TEL_OPT::SE ) == uint8_t( *( sub_option_end ) ) ) {
			it_end = sub_option_end + 1;
			return true;
		}

		return false;
	};

	static auto process_sub_option = []( le_console_o::connection_t* connection, std::string::const_iterator it, std::string::const_iterator const it_end ) -> bool {
		it++; // Move past the SB Byte
		logger.info( "Suboption x%02x (%1$03u)", *it );

		// LINEMODE SUBOPTION
		if ( uint8_t( *it ) == 34 ) {
			logger.debug( "\t Suboption LINEMODE" );

			it++; // Move past the suboption specifier

			if ( uint8_t( *it ) == 3 ) { // SLC = set local characters
				it++;
				logger.debug( "\t LINEMODE suboption SLC" );
				enum class SLC_LEVEL : uint8_t {
					NOSUPPORT  = 0,
					CANTCHANGE = 1,
					VALUE      = 2,
					DEFAULT    = 3,
				};
				static constexpr uint8_t SLC_LEVEL_BITS = 3; // mask for the level bits
				static constexpr uint8_t IAC            = '\xff';

				char const* level_str[ 4 ] = {
				    "NOSUPPORT  ",
				    "CANTCHANGE ",
				    "VALUE      ",
				    "DEFAULT    ",
				};

				while ( it < it_end ) {
					uint8_t triplet[ 3 ] = {};

					triplet[ 0 ] = *it;
					uint8_t( *it ) == IAC ? it += 2 : it++;
					if ( it >= it_end ) {
						break;
					}
					triplet[ 1 ] = *( it );
					uint8_t( *it ) == IAC ? it += 2 : it++;
					if ( it >= it_end ) {
						break;
					}
					triplet[ 2 ] = *( it );
					uint8_t( *it ) == IAC ? it += 2 : it++;
					if ( it >= it_end ) {
						break;
					}

					if ( triplet[ 0 ] > uint8_t( le_console_o::connection_t::CharTable::CharTable_invalid ) &&
					     triplet[ 0 ] < uint8_t( le_console_o::connection_t::CharTable::CharTable_last ) - 1 ) {

						switch ( triplet[ 1 ] & SLC_LEVEL_BITS ) {

						case ( uint8_t( SLC_LEVEL::CANTCHANGE ) ):
							break;
						case ( uint8_t( SLC_LEVEL::DEFAULT ) ):
							break;
						case ( uint8_t( SLC_LEVEL::NOSUPPORT ) ):
							connection->slc_remote[ triplet[ 0 ] ] = 0;
							break;
						case ( uint8_t( SLC_LEVEL::VALUE ) ):
							connection->slc_remote[ triplet[ 0 ] ] = triplet[ 2 ];
							break;
						default:
							break;
						}
					}

					logger.debug( "\t\t x%02x : %- 10s : x%02x", triplet[ 0 ], level_str[ triplet[ 1 ] & SLC_LEVEL_BITS ], triplet[ 2 ] );
				}
			}
		}

		return false;
	};

	static auto process_option = []( le_console_o::connection_t* connection, std::string::const_iterator it, std::string::const_iterator const it_end ) -> bool {
		if ( it + 1 >= it_end ) {
			return false;
		}

		// ----------| invariant: there is an option specifier available

		switch ( uint8_t( *it ) ) {
		case ( uint8_t( TEL_OPT::WILL ) ):
			logger.debug( "WILL x%02x (%1$03u)", *( it + 1 ) );

			// client will do something
			if ( uint8_t( *( it + 1 ) ) == 0x22 ) {
				// linemode activated on client -- we will operate in linemode from now on
				connection->state = le_console_o::connection_t::State::eTelnetLineMode;
				logger.debug( "LINEMODE activated on connection" );
			}

			break;
		case ( uint8_t( TEL_OPT::WONT ) ):
			logger.debug( "WONT x%02x (%1$03u)", *( it + 1 ) );
			break;
		case ( uint8_t( TEL_OPT::DO ) ):
			logger.debug( "DO   x%02x (%1$03u)", *( it + 1 ) );
			// client requests something from us
			break;
		case ( uint8_t( TEL_OPT::DONT ) ):
			logger.debug( "DONT x%02x (%1$03u)", *( it + 1 ) );
			break;
		}

		return true;
	};

	// std::string test_str = "this is \xff\xff\xff\xff a test \xff\xff hello, well \xffhere";
	// stream_begin                                      = test_str.begin();
	// std::string::const_iterator const test_stream_end = test_str.end();

	while ( true ) {

		auto prev_stream_begin = stream_begin;
		find_next_command( stream_begin, stream_end );

		{
			// add all characters which are not commands to the result stream
			// until we hit stream_begin
			bool iac_flip_flop = false;
			for ( auto c = prev_stream_begin; c != stream_begin; c++ ) {
				if ( *c == '\xff' ) {
					iac_flip_flop ^= true;
				}
				if ( !iac_flip_flop ) {

					if ( *c == '\x03' ) {
						connection->wants_close = true;
						return result;
					}

					result.push_back( *c );
				}
			}
		}

		if ( stream_begin == stream_end ) {
			break;
		}

		auto sub_range_end = stream_end;
		// check for possible commands:
		if ( is_option( stream_begin, sub_range_end ) ) {
			process_option( connection, stream_begin, sub_range_end );
			stream_begin = sub_range_end; // move it to end of the current range
		} else if ( is_sub_option( stream_begin, sub_range_end ) ) {
			process_sub_option( connection, stream_begin, sub_range_end );
			stream_begin = sub_range_end;
			// do something with suboption
		} else {
			// this is neither a suboption not an option
		};
	};

	return result;
}

// --------------------------------------------------------------------------------

static void le_console_process_input() {
	static auto logger = le::Log( LOG_CHANNEL );

	le_console_o* self = produce_console();

	if ( self == nullptr ) {
		logger.error( NO_CONSOLE_MSG );
		return;
	}

	auto connections_lock = std::scoped_lock( self->connections_mutex );

	for ( auto& c : self->connections ) {

		auto& connection = c.second;

		if ( connection->wants_close ) {
			// do no more processing if this connection wants to close
			continue;
		}

		std::string msg;

		connection->channel_in.fetch( msg );

		if ( msg.empty() ) {
			continue;
		}

		// --------| invariant: msg is not empty

		// Apply the telnet protocol - this means interpreting (updating the connection's telnet state)
		// and removing telnet commands from the message stream, and unescaping double-\xff "IAC" bytes to \xff.
		//
		msg = telnet_filter( connection.get(), msg.begin(), msg.end() );

		if ( connection->wants_close ) {
			// Do no more processing if this connection wants to close
			continue;
		}

		if ( msg.empty() ) {
			continue;
		}

		// Then, check if we are running in linemode
		if ( connection->state == le_console_o::connection_t::State::eTelnetLineMode ) {

			// Process virtual terminal control sequences - if we're in line mode we have to do these things
			// on the server-side.
			//
			// See: ECMA-48, <https://www.ecma-international.org/publications-and-standards/standards/ecma-48/>

			enum State {
				DATA,  // plain data
				ESC,   //
				CSI,   // ESC [
				ENTER, // '\r'
			};

			State state = State::DATA;

			union control_function_t {
				struct Bytes {
					char intro;
					char final;
				} bytes;
				uint16_t data = {};
			} control_function = {};

			// introducer and end byte of control function
			std::string parameters;
			bool        enter_user_input = false;

			static auto execute_control_function = []( le_console_o::connection_t* connection, control_function_t f, std::string const& parameters ) {
				// execute control function on connection based
				logger.debug( "executing control function: 0x%02x ('%1$c'), with parameters: '%2$s' and final byte: 0x%3$02x ('%3$c')", f.bytes.intro, parameters.c_str(), f.bytes.final );
			};

			for ( auto c : msg ) {

				switch ( state ) {
				case DATA:
					if ( c == '\x1b' ) {
						state = ESC;
					}
					if ( c == '\r' ) {
						state = ENTER;
					} else {

						if ( c == '\x7f' ) { // delete
							if ( !connection->input_buffer.empty() ) {
								// remove last character if not empty
								connection->input_buffer.resize( connection->input_buffer.size() - 1 );
								// todo: we must delete the character on the tty.
								connection->channel_out.post( "\b \b" );
								// connection->channel_out.post( "\x1b[6n" ); // query cursor position
							}
						} else {

							// TODO: Do something with plain data
							connection->input_buffer.push_back( c );
							connection->channel_out.post( std::string( 1, c ) );
						}
					}
					break;
				case ENTER:
					if ( c == 0x00 ) {
						enter_user_input = true;
						state            = DATA;
						connection->channel_out.post( "\r\n" ); // carriage-return+newline
					} else {
						connection->input_buffer.push_back( '\r' );
						state = DATA;
					}
					break;
				case ESC:
					if ( c == '\x5b' || c == '\x9b' ) { // 7-bit or 8 bit representation of control sequence
						state                        = CSI;
						control_function.bytes.intro = c;
					} else {
					}
					break;
				case CSI:
					if ( c >= '\x30' && c <= '\x3f' ) { // parameter bytes
						parameters.push_back( c );
					} else if ( c >= '\x20' && c <= '\x2f' ) { // intermediary bytes (' ' to '/')
						// we want to add these to the parameter string that we capture
						parameters.push_back( c );
					} else if ( c >= '\x40' && c <= '\x7e' ) { // final byte of a control sequence
						// todo: we must do something with this control sequence
						control_function.bytes.final = c;
						execute_control_function( connection.get(), control_function, parameters );
						state            = DATA;
						control_function = {};
					}
					// if end of command
					// if parameter byte
					break;
				}
				if ( enter_user_input ) {
					break;
				}
			}

			if ( enter_user_input ) {
				std::swap( connection->input_buffer, msg );
				connection->input_buffer.clear();
			} else {
				msg.clear();
				continue; // this stops processing the message any further.
			}

			// echo input if needed
		}

		// first, we want to process telnet control
		//      Telnet control means to respond to telnet events, and the IAC byte (255)
		// it also means to translate erase character and erase line etc. to local terminal
		//
		// then we want to process terminal control.
		//
		// how do we know if a message was user-initiated?

		// --------| invariant: message does not begin with \xff or \x1b

		char const* delim   = "\n\r= ";
		char*       context = nullptr;
		char*       token   = strtok_reentrant( msg.data(), delim, &context );

		std::vector<char const*> tokens;

		while ( token != nullptr ) {
			tokens.emplace_back( token );
			token = strtok_reentrant( nullptr, delim, &context );
		}

		if ( tokens.empty() ) {
			continue;
		}

		// ---------| invariant: tokens are not empty: process tokens

		switch ( hash_32_fnv1a( tokens[ 0 ] ) ) {
		case hash_32_fnv1a_const( "settings" ):
			le_log::le_log_channel_i.info( logger.getChannel(), "Listing Settings" );
			le::Settings().list();
			break;
		case hash_32_fnv1a_const( "set" ):
			if ( tokens.size() == 3 ) {
				le::Settings::set( tokens[ 1 ], tokens[ 2 ] );
			}
			break;
		case hash_32_fnv1a_const( "json" ): {
			// directly put a message onto the output buffer - without mirroring it to the console
			connection->channel_out.post( R"({ "Token": "This message should pass through unfiltered" }\r\n)" );
		} break;
		case hash_32_fnv1a_const( "cls" ): {

			connection->channel_out.post(
			    "\xff" // IAC
			    "\xfd" // DO
			    "\x22" // LINEMODE
			    ""
			    "\xff" // IAC
			    "\xfb" // WILL
			    "\x01" // ECHO
			);

			connection->channel_out.post( "\x1b[38;5;220mIsland Console.\r\nWelcome.\x1b[0m\r\n" );
			// connection->channel_out.post( "\x1b[H\x1b[2J" );
			// connection->channel_out.post( "\x1b[6n" ); // query current cursor position -- not yet sure how to interpret response.

			// connection->channel_out.post( "\x1b[4B" ); // move cursor down by 4 rows

		} break;
		case hash_32_fnv1a_const( "log" ):
			// If you set log_level_mask to 0, this means that log messages will be mirrored to console
			// If you set log_level_mask to -1, this means that all log messages will be mirrored to console
			if ( tokens.size() == 2 ) {
				le_console_produce_log_subscribers().erase( c.first ); // Force the subscriber to be de-registered.
				connection->log_level_mask = strtol( tokens[ 1 ], nullptr, 0 );
				if ( connection->log_level_mask != 0 ) {
					le_console_produce_log_subscribers()[ c.first ] = std::make_unique<LeLogSubscriber>( connection.get() ); // Force the subscriber to be re-registered.
					connection->wants_log_subscriber                = true;
				} else {
					// if we don't subscribe to any messages, we might as well remove the subscriber from the log
					le_console_produce_log_subscribers()[ c.first ].reset( nullptr );
					connection->wants_log_subscriber = false;
				}
				le_log::le_log_channel_i.info( logger.getChannel(), "Updated console log level mask to 0x%x", connection->log_level_mask );
			}
			break;
		default:
			le_log::le_log_channel_i.warn( logger.getChannel(), "Did not recognise command: '%s'", tokens[ 0 ] );
			break;
		}
	}
}

// ----------------------------------------------------------------------

static le_console_o* le_console_create() {
	static auto logger = le::Log( LOG_CHANNEL );
	logger.set_level( le::Log::Level::eDebug );
	auto self = new le_console_o();
	return self;
}

// ----------------------------------------------------------------------

static void le_console_destroy( le_console_o* self ) {
	static auto logger = le::Log( LOG_CHANNEL );

	// Tear-down and delete server in case there was a server
	le_console_server_stop();
	// we can do this because le_log will never be reloaded as it is part of the core.

	logger.info( "Destroying console..." );
	delete self;
}

// ----------------------------------------------------------------------

static void le_console_inc_use_count() {

	le_console_o* self = produce_console();

	if ( self == nullptr ) {
		*PP_CONSOLE_SINGLETON = le_console_create();
		self                  = produce_console();
	}

	self->use_count++;
}

// ----------------------------------------------------------------------

static void le_console_dec_use_count() {
	le_console_o* self = produce_console();
	if ( self ) {
		if ( --self->use_count == 0 ) {
			le_console_destroy( self );
			*PP_CONSOLE_SINGLETON = nullptr;
		};
	}
}

// ----------------------------------------------------------------------

LE_MODULE_REGISTER_IMPL( le_console, api_ ) {

	auto  api          = static_cast<le_console_api*>( api_ );
	auto& le_console_i = api->le_console_i;

	le_console_i.inc_use_count = le_console_inc_use_count;
	le_console_i.dec_use_count = le_console_dec_use_count;
	le_console_i.server_start  = le_console_server_start;
	le_console_i.server_stop   = le_console_server_stop;
	le_console_i.process_input = le_console_process_input;

	// Load function pointers for private server object
	le_console_server_register_api( &le_console_server_api );

	PP_CONSOLE_SINGLETON = le_core_produce_dictionary_entry( hash_64_fnv1a_const( "le_console_singleton" ) );

	if ( *PP_CONSOLE_SINGLETON ) {
		// if a console already exists, this is a sign that this module has been
		// reloaded - in which case we want to re-register the subscriber to the log.
		le_console_o* console = produce_console();
		if ( console->server ) {
			le_console_produce_server_watcher( console );
		}
		if ( !console->connections.empty() ) {
			auto  lock        = std::scoped_lock( console->connections_mutex );
			auto& subscribers = le_console_produce_log_subscribers();
			for ( auto& c : console->connections ) {
				if ( c.second->wants_log_subscriber ) {
					subscribers[ c.first ] = std::make_unique<LeLogSubscriber>( c.second.get() );
				}
			}
		}
	}
}