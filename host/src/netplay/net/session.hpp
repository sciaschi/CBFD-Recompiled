// A netplay session over GameNetworkingSockets: the host listens on a UDP port and the
// others connect to it directly, by its join code (its address, port and the match's key). Connections
// are encrypted by the library (they aren't authenticated: there is no certificate authority, so someone
// in the path could still pose as the host). All that's sent are the packets in packets.hpp, reliably
// (resent until they arrive, in order); the library closes a connection that sends a bigger message, and
// buffers little from each peer.
//
// Or through ICE (signalling.hpp): the host listens for peer-to-peer connections, reached by its id
// (local_id()) through a signalling server, and the library finds a way between them: directly through
// NAT, or relayed by a TURN server (Ice), so neither sees the other's address. Everything after that is the
// same. The host keeps its listen socket and its signaller for as long as the session lasts.
//
// The handshake: a joiner sends Hello (the match's key, then build, ROM, settings profile and mods
// hashes) within 3 s of its connection coming up. The host closes a wrong key at once, telling it nothing
// else; then it refuses any other difference, or a full or started match, and otherwise gives it the
// lowest free ports, one for each player on its machine (the Hello says how many; the host's players have
// the first ports, from 0), with Welcome. Once every port is taken, the host starts the match
// (Start: with the input delay); or before, with the ports still free open (Start's open). Once started, a
// player who left it (not kicked) may come back to their ports: the same player id, with as many players, is
// welcomed as rejoining (Welcome's rejoin); and a new player may join it on free ports (open ones, or those whose
// player left over seat_hold seconds ago). Either gets nothing of the match until lockstep brings them in (back,
// then welcome_back). Then each peer sends its own ports' pads and
// movement (Inputs) and memory hashes (Hash) to the host, which passes Inputs on to the others: the host is
// the hub, so joiners only ever connect to it. A peer that sends anything else (another peer's port's
// inputs, a second Hello) is dropped. After a desync the host may send a joiner its game's state (Resync,
// StatePart): the one message bigger than the rest, which a joiner takes from the host only.
//
// Spectators: a joiner with no players (Hello's 0) watches, if the host takes spectators (up to max_spectators,
// besides the players). It gets no ports, a spectator number of the host's instead (Event's `spectator`), and
// everything the match sends but sends nothing but its hashes; nobody waits for it. Before the start it gets Start
// as the players do; under way, it's brought in as a new player is (welcome_back, `spectators`), with the host's
// state. After a desync, the host sends a spectator its state alone (resync_spectators), the players none the wiser.
//
// Single-threaded: call poll() often (each controller read, or in a loop) from one thread; it runs the
// library's callbacks and returns what happened.

#pragma once

#include <array>
#include <deque>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "packets.hpp"

namespace conker::netplay {
    class Signaller;
}

namespace conker::netplay {
    struct Event {
        enum class Type {
            joined,     // host: a joiner passed the handshake, on `ports` (`rejoin`: into a match under way, `late`: new to it),
                        // or as spectator `spectator`
            welcomed,   // joiner: accepted, on `ports` (none: a spectator), in a match of `players` (`rejoin`: under way,
                        // with its rules; `late`: as a new player, or spectator)
            started,    // joiner: the match starts (`open`: the ports nobody has)
            packet,     // `packet` arrived (Inputs, Hash, or for a joiner the host's Gone or Delay), from `port`'s peer
                        // (via the host for a joiner; Gone: the port gone), or the host's from spectator `spectator`'s (Hash)
            left,       // host: the peer on `ports` (or spectator `spectator`) left or was dropped, for `reason`
            ended,      // joiner: refused, or the connection to the host ended, for `reason`
            moved,      // joiner, before the start: the host moved this machine to `ports` (none: watching), its roster says;
                        // `late`: its step-out crossed the Start (its places were `port`'s bits: the host says they're idle)
        };
        Type type;
        int port = -1;          // the peer's first port (packet: the Inputs' port, for a joiner)
        int spectator = -1;     // host: joined, left, packet: the peer is that spectator (0 to max_spectators - 1)
        uint64_t player = 0;    // host: packet: the peer's player id
        uint8_t ports = 0;      // joined, welcomed, left: the peer's ports (bit n: port n)
        int players = 0;
        bool rejoin = false;    // joined, welcomed: into a match under way (coming back, or `late`: a new player)
        bool late = false;
        uint8_t open = 0;       // started: the ports nobody has (idle from read 0)
        bool own_view = false;  // started (and welcomed back): the host's rule that every player sees only their own view
        std::array<uint8_t, max_players> delays{};  // started: each port's input delay
        uint8_t pairs = 0;      // started (and welcomed back): the ports shown two to a window (each machine with two players')
        int rollback = 0;       // started (and welcomed back): the reads every peer predicts ahead (0: off)
        int characters = 0;     // started (and welcomed back): the most characters in a multiplayer game (0: the game's own)
        Reason reason = Reason::quit;
        std::string detail;     // the library's account of a closed connection, for the log
        std::string release;    // ended by a Refuse: the host's release, if it said (another release, to get)
        bool by_signalling = false; // ended: the signalling server's word (the host isn't there, or the
                                    // server's gone), and `detail` says it for the player
        Packet packet;
        // packet, a joiner's Inputs: an early copy, sent unreliably (Session::send_to), which may repeat reads that came
        // already, or start past them (one lost on the way: it's for the reliable Inputs to fill in, so it's not taken).
        bool copy = false;
        // joined (host): a machine whose ports the host let go of as their inputs stopped coming (let_go_of) is here
        // again on the same connection (its Claim), for the host's game (as `rejoin`).
        bool returned = false;
        // packet, Inputs: when the library got it (seconds, on the steady clock: lockstep::now's), which may be well
        // before it's polled.
        double arrived = 0;
    };

    // The join code (join_code.hpp) for `address` ("1.2.3.4:5000" or "[::1]:5000") and a match's `key`, or
    // empty if it isn't an address and port.
    std::string join_code_for(const std::string& address, uint64_t key);

    // Starts and stops the library, once per process, around any sessions. Starting makes this process's
    // id: random, for the library's identity and to be reached by through a signalling server.
    bool startup(std::string& error);
    void shutdown();
    // This process's id (never 0, once started).
    uint64_t local_id();
    // Gives this process a new id (and the library that identity), closing every connection: for each new
    // match through a signalling server, which doesn't give an id out again for an hour after it's left.
    bool new_id(std::string& error);
    // 64 bits from the system's random source (never 0): a match's key, a player id. False if there's none.
    bool random_key(uint64_t& key);

    // How ICE connections find their way: the servers the library asks (STUN for this machine's public
    // address, TURN to relay), as "host:port" lists separated by commas (empty: none), and which ways are
    // allowed. `relay_only`: only through a TURN server, so no peer is told another's address (and without
    // one there is no way); otherwise directly too, when the networks allow it, the relay if not.
    struct Ice {
        std::string stun;
        std::string turn;
        std::string turn_user;
        std::string turn_password;
        bool relay_only = true;
    };

    // A bad network, faked by the library for tests (lockstep's --netplay-fake-network): what this process
    // sends is held back `lag_ms`, plus jitter (an average of `jitter_ms`, at most 4 times that), `loss`% of
    // its packets dropped, `reorder`% held back 20 ms more (so they arrive out of order) and `dup`% sent twice.
    // `outage`: everything this process sends and receives is dropped, until it's set false.
    struct FakeNetwork {
        int lag_ms = 0;
        float jitter_ms = 0;
        float loss = 0;
        float reorder = 0;
        float dup = 0;
        bool outage = false;
        bool operator==(const FakeNetwork&) const = default;
    };
    // Sets it for every connection, once the library is started.
    void set_fake_network(const FakeNetwork& fake);

    class Session {
    public:
        Session();
        ~Session();
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        // Hosts a match of `players` (2 to 4) on UDP `port`, on every local address, with a new random key;
        // `local` of them (fewer than `players`) play on this machine, on the first ports.
        bool host(uint16_t port, int players, const Hello& me, std::string& error, int local = 1);
        // Host: the join code for this match at `address` (the host's, as the others reach it), or empty
        // if it isn't an address and port.
        std::string join_code(const std::string& address) const;
        // Joins the host by its join code (`me`'s key is the code's; its players, this machine's).
        bool join(const std::string& code, const Hello& me, std::string& error);
        // The same through ICE: hosts, reached by local_id() through `signaller` (connected, or connecting),
        // or joins the host with id `host_id` and the match's `key`. The signaller must outlive the session;
        // poll() polls it too.
        bool host(Signaller& signaller, const Ice& ice, int players, const Hello& me, std::string& error, int local = 1);
        bool join(Signaller& signaller, const Ice& ice, uint64_t host_id, uint64_t key, const Hello& me, std::string& error);
        // Host: the match's key, for a joiner through ICE (with local_id()).
        uint64_t key() const { return is_host ? hello.key : 0; }
        // Host: once every port is taken (false until then; or at once with `open_seats`, the free ports open), starts
        // the match: tells the joiners, and takes new players only on free ports from then on. `own_view`: every player
        // sees only their own view (the match's rule); `delays`: each port's input delay; `rollback`: the reads
        // every peer predicts ahead (0: off); `characters`: the most in a multiplayer game (packets.hpp's valid_characters).
        bool start(bool own_view = false, const std::array<uint8_t, max_players>& delays = { 2, 2, 2, 2 }, int rollback = 0,
            bool open_seats = false, int characters = 0);
        // The waiting room, before the start (packets.hpp's Roster and Ready). Host: the match's settings it
        // shows (`delay` -1: auto), whether its own players are ready (its Start), the players it keeps out
        // (their player ids: refused as kicked), and kick, which drops a port's machine (kicked; false if no
        // joiner has it). Joiner: set_ready tells the host. roster(): as the host last sent it (the host's:
        // as it is now; it's sent in the match too); everyone_ready: host, every machine ready, and every port taken
        // (unless not `full`). `spectators`: the host takes them (they're never waited for).
        void set_room_settings(bool own_view, int delay, int rollback, bool spectators = true, int characters = 0);
        void set_ready(bool ready);
        void set_blocked(const std::vector<uint64_t>& players) { blocked = players; }
        // Host: this game's release (hello.hpp's release_name), told to each joiner it refuses (not if it
        // isn't 1 to max_release_name printable characters).
        void set_release(const std::string& name);
        // `keep_out` false: dropped as quitting, and may come back.
        bool kick(int port, bool keep_out = true);
        // Host: drops spectator `spectator` (kicked; with `keep_out`, not back in this match either), if `player` (0: any) is
        // who's there.
        bool kick_spectator(int spectator, uint64_t player, bool keep_out = true);
        Roster roster() const;
        bool everyone_ready(bool full = true) const;
        // Host: how long a player who left the match keeps their ports (to come back to) before a new one can take them.
        void set_seat_hold(double seconds) { seat_hold = seconds; }
        // Host: the round trip to each joiner, in milliseconds, as the library measures it (none it doesn't
        // know yet), for choosing the input delay.
        std::vector<int> round_trips_ms() const;
        // The round trip to each port's machine, in milliseconds, for the player to see (-1: not known, or not
        // another machine's port): the host's to every joiner's ports; a joiner's to the host, on port 0 (it
        // can't tell which other ports are the host's, and the other joiners' pads come through the host).
        std::array<int, max_players> round_trips_by_port() const;
        // How the connection to `port`'s machine goes, for the log: the address this end sends to (not through
        // ICE, where it may be the peer's own) and the library's description. A joiner's only peer is the host.
        std::string route(int port) const;
        // Host: the joiners in (machines, not players: one may have several ports).
        int joiners() const;
        // Host: the connections still on their way in (before their handshake is done).
        int arriving() const;
        // Host: the ports whose views are shown two to a window (each machine with two players', the host's too),
        // sent with Start.
        uint8_t pairs() const;

        // What has happened since the last poll.
        std::vector<Event> poll();

        // One of this peer's own ports' pads (a joiner's go to the host, the host's to everyone).
        void send(const Inputs& inputs);
        // This peer's memory hash (a joiner's go to the host, the host's to everyone).
        void send(const Hash& hash);
        // Host: a player has left, to the joiners still in (after their last inputs, passed on as they came).
        void send(const Gone& gone);
        // Host: the input delay changes (Delay), to the joiners still in.
        void send(const Delay& delay);
        // Host: desync recovery: the joiners on Resync's ports are to load the host's state, to every joiner
        // still in; then the state itself (compressed, `hash` its XXH3-64 decompressed), in StateParts to the joiners
        // on `ports`, at a higher rate than the rest until it's all gone.
        void send(const Resync& resync);
        // Host: rejoining. `ports` (rejoining joiners', welcomed; of them `late`, new players) play again from `read`:
        // Back to the joiners in the match (their inputs are taken from that read on). Then, once the host's game before it is taken,
        // welcome_back brings each of those joiners in: its Rejoin, the inputs since (`since`), and from then on what every
        // peer gets, but Inputs of reads before `inputs_from` (by port: those it has); its state follows (send_state).
        void back(uint8_t ports, uint32_t read, uint8_t late = 0, uint64_t player = 0);
        // Seat changes. Host: spectator `spectator` is the player on `port` from now on (a free port, or one held for
        // a player who left: they can't come back to it), as back() with its player id tells everyone in a match, the roster
        // before it (false: it can't). unseat: the machine on `ports` (all of a joiner's) has stepped out (its Unseat, or
        // the host's say before the start): it's a spectator from now on, its number returned (-1: none free, or none
        // taken, or no such machine). machine_of: the ports of the joiner's machine on `port` (0: none). ask_unseat: asks
        // that machine to step out (false: it can't). Joiner: claim asks the host for a free port (watching); step_out
        // tells it this machine's players have stepped out (its inputs end with those sent), watching from now on (before
        // the start, once the host's roster says so: moved); take_seat: the host's Back gave it `ports`.
        bool seat_spectator(int spectator, int port);
        int unseat(uint8_t ports);
        uint8_t machine_of(int port) const;
        bool ask_unseat(int port);
        void claim(int port);
        void step_out();
        void take_seat(uint8_t ports);
        // A machine's inputs that stopped coming for a while. Host: let_go_of the machine on `ports` (after the Gone
        // that tells everyone, it too): it's out of the match (nothing goes to it, what it sends is let be) but still
        // connected, till it says it's here again (a joined event, `returned`) and welcome_back brings it in. Joiner:
        // come_back, once the host's Gone for its own ports came (Gone's 3), asks for the host's game.
        void let_go_of(uint8_t ports);
        void come_back();
        // `spectators` (bit n: spectator n) watching likewise: those not yet in the match come in with welcome_back, and get
        // the state with send_state; resync_spectators tells those in it whose game went another way to load it before
        // `read` (Resync to them alone), and send_state sends it to them too.
        void welcome_back(uint8_t ports, uint8_t spectators, const Rejoin& rejoin, const std::vector<Inputs>& since,
            const std::array<uint32_t, max_players>& inputs_from);
        void send_state(uint8_t ports, uint8_t spectators, uint32_t read, const std::vector<uint8_t>& state, uint64_t hash);
        void resync_spectators(uint8_t spectators, uint32_t read);
        // Says goodbye to every peer and closes the connections, once what was sent has arrived (up to 2 s,
        // for a lossy network). Poll a moment before deleting the session or stopping the library.
        void leave(Reason reason);
        // The furthest read a peer's Inputs may reach (their first read plus their count); beyond it they're
        // refused as broken or hostile, before the host passes them on. Lockstep sets it as the game goes;
        // until then, the first reads only.
        void set_horizon(uint64_t reads) { horizon = reads; }

        bool hosting() const { return is_host; }
        uint8_t ports() const { return own_ports; }
        // Joiner: the host has welcomed it (on its ports, or as a spectator: watching).
        bool welcomed() const { return is_welcomed; }
        bool watching() const { return is_welcomed && own_ports == 0; }
        // Host: the spectators in (bit n: spectator n), and whether it takes more.
        uint8_t spectators() const;
        bool takes_spectators() const { return settings.spectators != 0; }
        // Host: the ports open as the match started (Start's open).
        uint8_t open_at_start() const { return match_open; }
        // Host, in the match: the ports a new player could take now (nobody on them, nor holding them).
        uint8_t free_ports() const;
        int players() const { return match_players; }

    private:
        struct Peer {
            uint32_t connection = 0;
            int port = -1;          // its first port; -1 until the handshake is done (and for a spectator)
            uint8_t ports = 0;      // all of them (bit n: port n)
            double connected_at = 0;   // host: when its connection came up (Connected)
            bool up = false;            // host: it has (the handshake time runs from then)
            int64_t last_hash = -1; // the read of the last Hash from it (one per read, in order)
            uint64_t player = 0;    // host: its Hello's player id and platform, and whether it's ready
            uint8_t platform = 0;
            bool ready = false;
            bool sending_state = false; // host: a state is on its way to it (at the higher rate)
            // Host: whether it gets the match (a rejoining joiner doesn't until welcome_back), and by port, the read the
            // Inputs it gets start at (the earlier ones it has, or doesn't need).
            bool in_match = true;
            std::array<uint32_t, max_players> inputs_from{};
            // The reliable messages but Inputs and Hashes sent to it, and taken from it (Inputs' `controls`).
            uint16_t controls_sent = 0;
            uint16_t controls_taken = 0;
            // Host: a new player under way (Welcome's 2), whether it has played (sent an input), and the claims on its
            // ports it replaced (a player who left them, and when), put back should it leave before it played.
            bool late = false;
            bool played = false;
            std::array<uint64_t, max_players> replaced_players{};
            std::array<double, max_players> replaced_at{};
            int spectator = -1;     // host: a spectator's number, from the handshake
            // Host: let go of as its inputs stopped coming (let_go_of), and whether it has asked to come back since.
            bool let_go = false;
            bool returning = false;
        };

        void on_status_changed(const void* info);
        void handle(Peer& peer, const Packet& packet, std::vector<Event>& events);
        // A message that arrived on `connection`: decoded and handled, or its peer dropped.
        void take(uint32_t connection, std::span<const uint8_t> data, std::vector<Event>& events);
        // Takes every message still unread from `connection` (a peer that closed it).
        void drain(uint32_t connection, std::vector<Event>& events);
        void drop(Peer& peer, Reason reason, std::vector<Event>* events);
        // A joiner not yet welcomed: Refuse (with this one's release), then closed.
        void refuse(Peer& peer, Reason reason);
        // Waits until every peer has acknowledged what was sent to it (up to 2 s).
        void flush();
        void send_to(uint32_t connection, const Packet& packet, bool reliable = true);
        // Host: `inputs` to `peer`, but the reads before its inputs_from (none, if it isn't in the match), with an early
        // copy of its port's recent reads.
        void send_inputs(const Peer& peer, const Inputs& inputs);
        // `inputs` (own, or passed on) into its port's recent reads, for the early copies.
        void remember_sent(const Inputs& inputs);
        // Host: a joiner left the match under way (`reason`): unless it was kicked, it may come back to its ports.
        void remember(const Peer& peer, Reason reason);
        // Whether something sent on `connection` hasn't arrived yet.
        bool pending(uint32_t connection) const;
        // A state's send rate and buffer on `connection` (`state`), or the library's own.
        void set_send_rate(uint32_t connection, bool state);
        Peer* find(uint32_t connection);
        // Host: through the handshake, a player's or a spectator's.
        static bool in(const Peer& peer) { return peer.port >= 0 || peer.spectator >= 0; }
        // The spectator a joiner's Hello (none playing) gets: the lowest number free (-1: none).
        int free_spectator() const;
        friend void status_changed_callback(void* info);

        // Hosting: the key, the poll group and the joiners' ports; listening is the caller's.
        bool begin_hosting(int players, const Hello& me, std::string& error, int local);
        // Passes the signaller's news to the library: signals, and a host that isn't there.
        void take_signals(std::vector<Event>& events);

        Signaller* signaller = nullptr;
        struct IceContext;
        std::unique_ptr<IceContext> ice_context;
        uint64_t ice_host = 0;      // joiner through ICE: the host's id
        bool is_host = false;
        bool entry_open = true;
        // Host: the match has started (its rules, for a player coming back), and this end has left it.
        bool match_started = false;
        bool closed = false;
        uint8_t match_own_view = 0;
        uint8_t match_pairs = 0;
        uint8_t match_rollback = 0;
        uint8_t match_open = 0;
        uint8_t match_characters = 0;
        // Host: by port, the player who left it in the match (0: none, or kicked), who may come back, and how often
        // each port has been come back to (a player can't make everyone wait for a state again and again).
        std::array<uint64_t, max_players> left_players{};
        std::array<double, max_players> left_at{};
        std::array<int, max_players> rejoins{};
        double seat_hold = 120.0;
        std::vector<uint64_t> kicked;   // host: the players kicked from the match under way (spectators too)
        bool rejoining = false;     // joiner: welcomed back to a match under way, and Rejoin not here yet
        bool started = false;       // joiner: Start has arrived
        uint8_t own_ports = 0;      // joiner: 0 until welcomed (and as a spectator)
        bool is_welcomed = false;
        bool stepping_out = false;  // joiner: stepped out before the start, the host's roster not here yet
        int match_players = 0;
        uint64_t horizon = 64;
        std::array<uint64_t, max_players> next_inputs{};   // host: the read each port's next Inputs start at
        // Each port's latest input_window reads sent (own, or passed on), from `recent_first`: every Inputs goes with an
        // unreliable copy of them, so a packet lost on a bad link doesn't hold up the inputs after it until it's resent
        // (the reliable ones arrive in order). The copy that comes first counts; the rest of what comes is cut to the
        // reads after it.
        std::array<std::deque<std::pair<Pad, Motion>>, max_players> recent;
        std::array<uint64_t, max_players> recent_first{};
        bool taking_unreliable = false;     // the message being taken came unreliably (an early copy)
        double taking_arrived = 0;          // ...and when the library got it (Event's arrived)
        Hello hello;
        std::string release;
        uint32_t listen_socket = 0;
        uint32_t poll_group = 0;
        std::vector<Peer> peers;            // host: everyone connected; joiner: the host
        std::vector<Event> pending_events;  // from the status callback, until the next poll
        // The waiting room: host, its own readiness, whom it keeps out, the settings shown, and the roster as
        // last sent (and when next, for the round trips); joiner, the roster as the host last sent it.
        bool own_ready = false;
        std::vector<uint64_t> blocked;
        Roster settings;
        Roster sent_roster;
        double next_roster = 0;
        Roster host_roster;
    };
}
