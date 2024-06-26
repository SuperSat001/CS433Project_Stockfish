/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2024 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "uci.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <memory>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#include "benchmark.h"
#include "evaluate.h"
#include "movegen.h"
#include "nnue/network.h"
#include "nnue/nnue_common.h"
#include "perft.h"
#include "position.h"
#include "search.h"
#include "syzygy/tbprobe.h"
#include "types.h"
#include "ucioption.h"

namespace Stockfish {

constexpr auto StartFEN  = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
constexpr int  MaxHashMB = Is64Bit ? 33554432 : 2048;


namespace NN = Eval::NNUE;


UCI::UCI(int argc, char** argv) :
    networks(NN::Networks(
      NN::NetworkBig({EvalFileDefaultNameBig, "None", ""}, NN::EmbeddedNNUEType::BIG),
      NN::NetworkSmall({EvalFileDefaultNameSmall, "None", ""}, NN::EmbeddedNNUEType::SMALL))),
    cli(argc, argv) {

    options["Debug Log File"] << Option("", [](const Option& o) { start_logger(o); });

    options["Threads"] << Option(1, 1, 1024, [this](const Option&) {
        threads.set({options, threads, tt, networks});
    });

    options["Hash"] << Option(16, 1, MaxHashMB, [this](const Option& o) {
        threads.main_thread()->wait_for_search_finished();
        tt.resize(o, options["Threads"]);
    });

    options["Clear Hash"] << Option([this](const Option&) { search_clear(); });
    options["Ponder"] << Option(false);
    options["MultiPV"] << Option(1, 1, MAX_MOVES);
    options["Skill Level"] << Option(20, 0, 20);
    options["Move Overhead"] << Option(10, 0, 5000);
    options["nodestime"] << Option(0, 0, 10000);
    options["UCI_Chess960"] << Option(false);
    options["UCI_LimitStrength"] << Option(false);
    options["UCI_Elo"] << Option(1320, 1320, 3190);
    options["UCI_ShowWDL"] << Option(false);
    options["SyzygyPath"] << Option("<empty>", [](const Option& o) { Tablebases::init(o); });
    options["SyzygyProbeDepth"] << Option(1, 1, 100);
    options["Syzygy50MoveRule"] << Option(true);
    options["SyzygyProbeLimit"] << Option(7, 0, 7);
    options["EvalFile"] << Option(EvalFileDefaultNameBig, [this](const Option& o) {
        networks.big.load(cli.binaryDirectory, o);
    });
    options["EvalFileSmall"] << Option(EvalFileDefaultNameSmall, [this](const Option& o) {
        networks.small.load(cli.binaryDirectory, o);
    });

    networks.big.load(cli.binaryDirectory, options["EvalFile"]);
    networks.small.load(cli.binaryDirectory, options["EvalFileSmall"]);

    threads.set({options, threads, tt, networks});

    search_clear();  // After threads are up
}

void UCI::loop() {

    Position     pos;
    std::string  token, cmd;
    StateListPtr states(new std::deque<StateInfo>(1));

    pos.set(StartFEN, false, &states->back());

    for (int i = 1; i < cli.argc; ++i)
        cmd += std::string(cli.argv[i]) + " ";

    do
    {
        if (cli.argc == 1
            && !getline(std::cin, cmd))  // Wait for an input or an end-of-file (EOF) indication
            cmd = "quit";

        std::istringstream is(cmd);

        token.clear();  // Avoid a stale if getline() returns nothing or a blank line
        is >> std::skipws >> token;

        if (token == "CS433" || token == "cs433")
            cs433_project(pos, is, states);

        else if (token == "quit" || token == "stop")
            threads.stop = true;

        // The GUI sends 'ponderhit' to tell that the user has played the expected move.
        // So, 'ponderhit' is sent if pondering was done on the same move that the user
        // has played. The search should continue, but should also switch from pondering
        // to the normal search.
        else if (token == "ponderhit")
            threads.main_manager()->ponder = false;  // Switch to the normal search

        else if (token == "uci")
            sync_cout << "id name " << engine_info(true) << "\n"
                      << options << "\nuciok" << sync_endl;

        else if (token == "setoption")
            setoption(is);
        else if (token == "go")
            go(pos, is, states);
        else if (token == "position")
            position(pos, is, states);
        else if (token == "ucinewgame")
            search_clear();
        else if (token == "isready")
            sync_cout << "readyok" << sync_endl;

        // Add custom non-UCI commands, mainly for debugging purposes.
        // These commands must not be used during a search!
        else if (token == "flip")
            pos.flip();
        else if (token == "bench")
            bench(pos, is, states);
        else if (token == "d")
            sync_cout << pos << sync_endl;
        else if (token == "eval")
            trace_eval(pos);
        else if (token == "compiler")
            sync_cout << compiler_info() << sync_endl;
        else if (token == "export_net")
        {
            std::pair<std::optional<std::string>, std::string> files[2];

            if (is >> std::skipws >> files[0].second)
                files[0].first = files[0].second;

            if (is >> std::skipws >> files[1].second)
                files[1].first = files[1].second;

            networks.big.save(files[0].first);
            networks.small.save(files[1].first);
        }
        else if (token == "--help" || token == "help" || token == "--license" || token == "license")
            sync_cout
              << "\nStockfish is a powerful chess engine for playing and analyzing."
                 "\nIt is released as free software licensed under the GNU GPLv3 License."
                 "\nStockfish is normally used with a graphical user interface (GUI) and implements"
                 "\nthe Universal Chess Interface (UCI) protocol to communicate with a GUI, an API, etc."
                 "\nFor any further information, visit https://github.com/official-stockfish/Stockfish#readme"
                 "\nor read the corresponding README.md and Copying.txt files distributed along with this program.\n"
              << sync_endl;
        else if (!token.empty() && token[0] != '#')
            sync_cout << "Unknown command: '" << cmd << "'. Type help for more information."
                      << sync_endl;

    } while (token != "quit" && cli.argc == 1);  // The command-line arguments are one-shot
}

float UCI::curr_centipawn_eval_value(Stockfish::Position &pos){
    Value v = Eval::evaluate(networks,pos,VALUE_ZERO);
    int curr_cp_eval = UCI::to_cp(v,pos);
    return 0.01*curr_cp_eval;
}

bool move_to_be_skipped(const Move &m){
    return m.to_sq() >= 48 || !(m.type_of() == NORMAL);
}

//write code here for CS433 project
void UCI::cs433_project(Stockfish::Position& pos, std::istringstream& is, Stockfish::StateListPtr& states){

    //compute relevant board configuration where 4 pieces are relocated, by performing a state space search over the staring board configuration

    //call the neural network evaluation function and get the score for white

    //print out to sync_cout stream the FEN enconding of best board configuration with the score

    std::string token;
    is >> token;

    // We are calculating evaluations using 
    // custom made centipawn loss evaluation function (which uses the evaluate() function)
    // White to play at the end

    float max_val = 0;
    std::string best_fen;

    if(token == "1"){

        /*
            Assumptions:
            (1) Only moving White pieces to free squares
            (2) We don't move the White King
            (3*) Don't move white pawns since they can't do much at higher ranks without support
            (4) Checks and captures are not allowed (details in report)
        */

       sync_cout<<"Searching across all moves!\n"<<sync_endl;
        

        // List of free squares where pieces can move

        Square free_sq[] ={
        SQ_A3, SQ_B3, SQ_C3, SQ_D3, SQ_E3, SQ_F3, SQ_G3, SQ_H3,
        SQ_A4, SQ_B4, SQ_C4, SQ_D4, SQ_E4, SQ_F4, SQ_G4, SQ_H4,
        SQ_A5, SQ_B5, SQ_C5, SQ_D5, SQ_E5, SQ_F5, SQ_G5, SQ_H5,
        SQ_A6, SQ_B6, SQ_C6, SQ_D6, SQ_E6, SQ_F6, SQ_G6, SQ_H6,
        };

        int end_sq_size = 32;


        Square start_sq[]  = {
        SQ_A1, SQ_B1, SQ_C1, SQ_D1, SQ_F1, SQ_G1, SQ_H1,
        // SQ_A2, SQ_B2, SQ_C2, SQ_D2, SQ_E2, SQ_F2, SQ_G2, SQ_H2,
        };

        int start_sq_size = 7;
        
        for(int sq1 = 0; sq1 < start_sq_size; sq1++){
            for(int sq2 = sq1+1; sq2 < start_sq_size; sq2++){
                for(int sq3 = sq2+1; sq3 < start_sq_size; sq3++){
                    for(int sq4 = sq3+1; sq4 < start_sq_size; sq4++){
                        // We have selected 4 starting squares to remove the pieces from

                        int end1,end2,end3,end4;
                        for( end1 = 0; end1 < end_sq_size; end1++){
                            for( end2 = end1 + 1; end2 <end_sq_size; end2++ ){
                                for( end3 = end2 + 1; end3 < end_sq_size; end3++){
                                    for( end4 = end3+1; end4 < end_sq_size; end4++){
                                        // We have selected 4 ending squares to put the piece back on

                                        // Making changes to the current position
                                        Move m1 = Move(start_sq[sq1],free_sq[end1]);
                                        states->emplace_back();
                                        pos.do_move_433(m1,states->back());
                                        Move m2 = Move(start_sq[sq2],free_sq[end2]);
                                        states->emplace_back();
                                        pos.do_move_433(m2,states->back());
                                        Move m3 = Move(start_sq[sq3],free_sq[end3]);
                                        states->emplace_back();
                                        pos.do_move_433(m3,states->back());
                                        Move m4 = Move(start_sq[sq4],free_sq[end4]);
                                        states->emplace_back();
                                        pos.do_move_433(m4,states->back());

                                        // Computing evaluation (white to play)
                                        float val = curr_centipawn_eval_value(pos);
                                        
                                        if(val > max_val){
                                            max_val = val;
                                            best_fen = pos.fen();
                                        }

                                        // restoring pos variable
                                        pos.undo_move(m4);
                                        pos.undo_move(m3);
                                        pos.undo_move(m2);
                                        pos.undo_move(m1);
                                        states->pop_back();
                                        states->pop_back();
                                        states->pop_back();
                                        states->pop_back();
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

    
    }
    else if(token == "2"){

        float curr_cp_eval;
        /*
            Assumptions:
            (1) Only moving White pieces (giving white 4 extra moves at the start)
            (2) Captures at g or h rank are not allowed (details in report)
            (3) Castling not allowed
        */

        sync_cout<<"Searching across 4 legal moves!\n"<<sync_endl;

        for (const auto& m1 : MoveList<LEGAL>(pos)){
            if(move_to_be_skipped(m1)) continue;

            states->emplace_back();

            // sync_cout<<"First Move:"<<sync_endl;
            pos.do_move(m1, states->back());
            pos.sideToMove = ~ pos.sideToMove;

            for (const auto& m2 : MoveList<LEGAL>(pos)){
                if(move_to_be_skipped(m2)) continue;

                states->emplace_back();

                // sync_cout<<"Second Move:"<<sync_endl;
                pos.do_move(m2, states->back());
                pos.sideToMove = ~ pos.sideToMove;


                for (const auto& m3 : MoveList<LEGAL>(pos)){
                    if(move_to_be_skipped(m3)) continue;

                    states->emplace_back();

                    // sync_cout<<"Third Move:"<<sync_endl;
                    pos.do_move(m3, states->back());
                    pos.sideToMove = ~ pos.sideToMove;
        

                    for (const auto& m4 : MoveList<LEGAL>(pos)){
                        if(move_to_be_skipped(m4)) continue;

                        states->emplace_back();
        
                        // sync_cout<<"Fourth Move:"<<sync_endl;
                        
                        pos.do_move(m4, states->back());
                        pos.sideToMove = ~ pos.sideToMove;
        
                        // Calculate the current NNUE eval
                        curr_cp_eval = UCI::curr_centipawn_eval_value(pos);
                        
                        // If the current eval is better than the best eval so far, update the best eval and the best FEN
                        if(curr_cp_eval > max_val){
                            max_val = curr_cp_eval;
                            best_fen = pos.fen();
                        }

                        pos.undo_move(m4);
                        pos.sideToMove = ~ pos.sideToMove;
                        states->pop_back();
                    }
                    pos.undo_move(m3);
                    pos.sideToMove = ~ pos.sideToMove;
                    states->pop_back();
                }
                pos.undo_move(m2);
                pos.sideToMove = ~ pos.sideToMove;
                states->pop_back();
            }

            pos.undo_move(m1);
            pos.sideToMove = ~ pos.sideToMove;
            states->pop_back();
        }
    }
    else{

        sync_cout<<"Invalid choice! Exiting...\n"<<sync_endl;

        sync_cout<<"Usage: CS433 <choice>"<<sync_endl;
        sync_cout<<"<choice> = 1 or 2"<<sync_endl;
        sync_cout<<"1: Search across any 4 replacements"<<sync_endl;
        sync_cout<<"2: Search across 4 replacements which are legal moves\n"<<sync_endl;


        return;

    }

    
    // Print the best evaluation found
    sync_cout<<"Best NNUE eval is "<<max_val<< " (white side)\n" <<sync_endl;

    // Print the best board found
    Position best_pos;
    best_pos.set(best_fen, options["UCI_Chess960"], pos.state());
    sync_cout<<best_pos<<sync_endl;

    return;
}

Search::LimitsType UCI::parse_limits(const Position& pos, std::istream& is) {
    Search::LimitsType limits;
    std::string        token;

    limits.startTime = now();  // The search starts as early as possible

    while (is >> token)
        if (token == "searchmoves")  // Needs to be the last command on the line
            while (is >> token)
                limits.searchmoves.push_back(to_move(pos, token));

        else if (token == "wtime")
            is >> limits.time[WHITE];
        else if (token == "btime")
            is >> limits.time[BLACK];
        else if (token == "winc")
            is >> limits.inc[WHITE];
        else if (token == "binc")
            is >> limits.inc[BLACK];
        else if (token == "movestogo")
            is >> limits.movestogo;
        else if (token == "depth")
            is >> limits.depth;
        else if (token == "nodes")
            is >> limits.nodes;
        else if (token == "movetime")
            is >> limits.movetime;
        else if (token == "mate")
            is >> limits.mate;
        else if (token == "perft")
            is >> limits.perft;
        else if (token == "infinite")
            limits.infinite = 1;
        else if (token == "ponder")
            limits.ponderMode = true;

    return limits;
}

void UCI::go(Position& pos, std::istringstream& is, StateListPtr& states) {

    Search::LimitsType limits = parse_limits(pos, is);

    networks.big.verify(options["EvalFile"]);
    networks.small.verify(options["EvalFileSmall"]);

    if (limits.perft)
    {
        perft(pos.fen(), limits.perft, options["UCI_Chess960"]);
        return;
    }

    threads.start_thinking(options, pos, states, limits);
}

void UCI::bench(Position& pos, std::istream& args, StateListPtr& states) {
    std::string token;
    uint64_t    num, nodes = 0, cnt = 1;

    std::vector<std::string> list = setup_bench(pos, args);

    num = count_if(list.begin(), list.end(),
                   [](const std::string& s) { return s.find("go ") == 0 || s.find("eval") == 0; });

    TimePoint elapsed = now();

    for (const auto& cmd : list)
    {
        std::istringstream is(cmd);
        is >> std::skipws >> token;

        if (token == "go" || token == "eval")
        {
            std::cerr << "\nPosition: " << cnt++ << '/' << num << " (" << pos.fen() << ")"
                      << std::endl;
            if (token == "go")
            {
                go(pos, is, states);
                threads.main_thread()->wait_for_search_finished();
                nodes += threads.nodes_searched();
            }
            else
                trace_eval(pos);
        }
        else if (token == "setoption")
            setoption(is);
        else if (token == "position")
            position(pos, is, states);
        else if (token == "ucinewgame")
        {
            search_clear();  // Search::clear() may take a while
            elapsed = now();
        }
    }

    elapsed = now() - elapsed + 1;  // Ensure positivity to avoid a 'divide by zero'

    dbg_print();

    std::cerr << "\n==========================="
              << "\nTotal time (ms) : " << elapsed << "\nNodes searched  : " << nodes
              << "\nNodes/second    : " << 1000 * nodes / elapsed << std::endl;
}

void UCI::trace_eval(Position& pos) {
    StateListPtr states(new std::deque<StateInfo>(1));
    Position     p;
    p.set(pos.fen(), options["UCI_Chess960"], &states->back());

    networks.big.verify(options["EvalFile"]);
    networks.small.verify(options["EvalFileSmall"]);


    sync_cout << "\n" << Eval::trace(p, networks) << sync_endl;
}

void UCI::search_clear() {
    threads.main_thread()->wait_for_search_finished();

    tt.clear(options["Threads"]);
    threads.clear();
    Tablebases::init(options["SyzygyPath"]);  // Free mapped files
}

void UCI::setoption(std::istringstream& is) {
    threads.main_thread()->wait_for_search_finished();
    options.setoption(is);
}

void UCI::position(Position& pos, std::istringstream& is, StateListPtr& states) {
    Move        m;
    std::string token, fen;

    is >> token;

    if (token == "startpos")
    {
        fen = StartFEN;
        is >> token;  // Consume the "moves" token, if any
    }
    else if (token == "fen")
        while (is >> token && token != "moves")
            fen += token + " ";
    else
        return;

    states = StateListPtr(new std::deque<StateInfo>(1));  // Drop the old state and create a new one
    pos.set(fen, options["UCI_Chess960"], &states->back());

    // Parse the move list, if any
    while (is >> token && (m = to_move(pos, token)) != Move::none())
    {
        states->emplace_back();
        pos.do_move(m, states->back());
    }
}

namespace {
std::pair<double, double> win_rate_params(const Position& pos) {

    int material = pos.count<PAWN>() + 3 * pos.count<KNIGHT>() + 3 * pos.count<BISHOP>()
                 + 5 * pos.count<ROOK>() + 9 * pos.count<QUEEN>();

    // The fitted model only uses data for material counts in [10, 78], and is anchored at count 58.
    double m = std::clamp(material, 10, 78) / 58.0;

    // Return a = p_a(material) and b = p_b(material), see github.com/official-stockfish/WDL_model
    constexpr double as[] = {-185.71965483, 504.85014385, -438.58295743, 474.04604627};
    constexpr double bs[] = {89.23542728, -137.02141296, 73.28669021, 47.53376190};

    double a = (((as[0] * m + as[1]) * m + as[2]) * m) + as[3];
    double b = (((bs[0] * m + bs[1]) * m + bs[2]) * m) + bs[3];

    return {a, b};
}

// The win rate model is 1 / (1 + exp((a - eval) / b)), where a = p_a(material) and b = p_b(material).
// It fits the LTC fishtest statistics rather accurately.
int win_rate_model(Value v, const Position& pos) {

    auto [a, b] = win_rate_params(pos);

    // Return the win rate in per mille units, rounded to the nearest integer.
    return int(0.5 + 1000 / (1 + std::exp((a - double(v)) / b)));
}
}

std::string UCI::to_score(Value v, const Position& pos) {
    assert(-VALUE_INFINITE < v && v < VALUE_INFINITE);

    std::stringstream ss;

    if (std::abs(v) < VALUE_TB_WIN_IN_MAX_PLY)
        ss << "cp " << to_cp(v, pos);
    else if (std::abs(v) <= VALUE_TB)
    {
        const int ply = VALUE_TB - std::abs(v);  // recompute ss->ply
        ss << "cp " << (v > 0 ? 20000 - ply : -20000 + ply);
    }
    else
        ss << "mate " << (v > 0 ? VALUE_MATE - v + 1 : -VALUE_MATE - v) / 2;

    return ss.str();
}

// Turns a Value to an integer centipawn number,
// without treatment of mate and similar special scores.
int UCI::to_cp(Value v, const Position& pos) {

    // In general, the score can be defined via the the WDL as
    // (log(1/L - 1) - log(1/W - 1)) / ((log(1/L - 1) + log(1/W - 1))
    // Based on our win_rate_model, this simply yields v / a.

    auto [a, b] = win_rate_params(pos);

    return std::round(100 * int(v) / a);
}

std::string UCI::wdl(Value v, const Position& pos) {
    std::stringstream ss;

    int wdl_w = win_rate_model(v, pos);
    int wdl_l = win_rate_model(-v, pos);
    int wdl_d = 1000 - wdl_w - wdl_l;
    ss << " wdl " << wdl_w << " " << wdl_d << " " << wdl_l;

    return ss.str();
}

std::string UCI::square(Square s) {
    return std::string{char('a' + file_of(s)), char('1' + rank_of(s))};
}

std::string UCI::move(Move m, bool chess960) {
    if (m == Move::none())
        return "(none)";

    if (m == Move::null())
        return "0000";

    Square from = m.from_sq();
    Square to   = m.to_sq();

    if (m.type_of() == CASTLING && !chess960)
        to = make_square(to > from ? FILE_G : FILE_C, rank_of(from));

    std::string move = square(from) + square(to);

    if (m.type_of() == PROMOTION)
        move += " pnbrqk"[m.promotion_type()];

    return move;
}


Move UCI::to_move(const Position& pos, std::string& str) {
    if (str.length() == 5)
        str[4] = char(tolower(str[4]));  // The promotion piece character must be lowercased

    for (const auto& m : MoveList<LEGAL>(pos))
        if (str == move(m, pos.is_chess960()))
            return m;

    return Move::none();
}

}  // namespace Stockfish
