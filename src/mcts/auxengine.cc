/*
  This file is part of Leela Chess Zero.
  Copyright (C) 2022 The LCZero Authors

  Leela Chess is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Leela Chess is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Leela Chess.  If not, see <http://www.gnu.org/licenses/>.

  Additional permission under GNU GPL version 3 section 7

  If you modify this Program, or any covered work, by linking or
  combining it with NVIDIA Corporation's libraries from the NVIDIA CUDA
  Toolkit and the NVIDIA CUDA Deep Neural Network library (or a
  modified version of those libraries), containing parts covered by the
  terms of the respective license agreement, the licensors of this
  Program grant you additional permission to convey the resulting work.
*/

#include "mcts/search.h"

#include <algorithm>
#include <boost/process.hpp>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <thread>

#include "mcts/node.h"
#include "neural/cache.h"
#include "neural/encoder.h"
#include "utils/fastmath.h"
#include "utils/random.h"

std::default_random_engine generator;
std::uniform_real_distribution<float> distribution(1,0);

namespace lczero {

  // Plan to go multithreaded with 1 + k instances of the helper
  // make 1 + k instances of auxengine_is_, auxengine_os_ and auxengine_c_
  // rewrite DoAuxEngine() so that it takes pointers to the specific auxengine_is_ and auxengine_os_.
  // Let the first thread/instance continuosly query root.
  // Let the subsequent threads/instances pop nodes from the shared queue and process them as they see fit.

  std::vector<std::unique_ptr<boost::process::ipstream>> Search::vector_of_ipstreams;
  std::vector<std::unique_ptr<boost::process::opstream>> Search::vector_of_opstreams;
  std::vector<std::unique_ptr<boost::process::child>> Search::vector_of_children;
  std::vector<bool> Search::vector_of_auxengine_ready_;

void Search::OpenAuxEngine() REQUIRES(threads_mutex_) {
  if (params_.GetAuxEngineFile() == "") return;
  for(int i = 0; i < params_.GetAuxEngineInstances(); i++){
    auxengine_threads_.emplace_back([this]() { AuxEngineWorker(); });
  }
}

void SearchWorker::AuxMaybeEnqueueNode(Node* n, int source) {
  // the caller (DoBackupUpdate()->DoBackupUpdateSingleNode()) has a lock on search_->nodes_mutex_, so no other thread will change n right now.

  // Since we take a lock below, have to check if search is stopped.
  if (search_->stop_.load(std::memory_order_acquire)){
    return;
  }

  // if (params_.GetAuxEngineVerbosity() >= 9) LOGFILE
  //     << "AuxMaybeEnqueueNode() picked node: " << n->DebugString() 
  //     << " for the persistent_queue_of_nodes which has size: "
  //     << search_->search_stats_->persistent_queue_of_nodes.size()
  //     << " The source was " << source;

  search_->auxengine_mutex_.lock();    
  n->SetAuxEngineMove(0xfffe); // magic for pending
  search_->search_stats_->persistent_queue_of_nodes.push(n);
  search_->search_stats_->source_of_queued_nodes.push(source);
  search_->auxengine_mutex_.unlock();
  search_->auxengine_cv_.notify_one();

}

void Search::AuxEngineWorker() {

  // aquire a lock auxengine_mutex_ to ensure no other thread is
  // modifying search_stats_->thread_counter or the vector_of_*
  // vectors
  auxengine_mutex_.lock();

  // Find out which thread we are by reading the thread_counter.

  // Don't increment the thread_counter before all global vectors are
  // initiated, or MaybeTriggerStop() in search.cc will try to write
  // to uninitiated adresses.
  
  long unsigned int our_index = search_stats_->thread_counter;

  // if our_index is greater than the size of the vectors then we now for sure we must start/initiate everything.
  // if our_index + 1 is equal to, or smaller than the size of the vectors then we can safely check search_stats_->vector_of_auxengine_ready_[our_index] and act if it is false

  if(our_index + 1 > vector_of_auxengine_ready_.size() ||
     (
      our_index + 1 <= vector_of_auxengine_ready_.size() &&
      ! vector_of_auxengine_ready_[our_index]
     )
   ) {
  
    // populate the global vectors. 
    vector_of_ipstreams.emplace_back(new boost::process::ipstream);
    vector_of_opstreams.emplace_back(new boost::process::opstream);
    vector_of_children.emplace_back(new boost::process::child);

    // Start the helper
    *vector_of_children[our_index] = boost::process::child(params_.GetAuxEngineFile(), boost::process::std_in < *vector_of_opstreams[our_index], boost::process::std_out > *vector_of_ipstreams[our_index]);

    // Record that we have started, so that we can skip this on the next invocation.
    vector_of_auxengine_ready_.push_back(false);

    auxengine_stopped_mutex_.lock();  
    auxengine_stopped_.push_back(true);
    auxengine_stopped_mutex_.unlock();      
    
    {
      std::string bar;
      // Thread zero uses a different parameter than the other threads
      if(our_index == 0){
	// at root, infinite exploration
	bar = params_.GetAuxEngineOptionsOnRoot();
      } else {
	// in-tree time based evaluations
	bar = params_.GetAuxEngineOptions();
      }
      std::istringstream iss(bar);
      std::string token;
      while(std::getline(iss, token, '=')) {
        std::ostringstream oss;
        oss << "setoption name " << token;
        std::getline(iss, token, ';');
        oss << " value " << token;
        LOGFILE << oss.str();
	*vector_of_opstreams[our_index] << oss.str() << std::endl;
      }
      *vector_of_opstreams[our_index] << "uci" << std::endl;      
    }
    std::string line;
    while(std::getline(*vector_of_ipstreams[our_index], line)) {
      LOGFILE << line;
      std::istringstream iss(line);
      std::string token;
      iss >> token >> std::ws;
      if (token == "uciok") {
        break;
      } else if (token == "option") {
        iss >> token >> std::ws;
        if (token == "name") {
          iss >> token >> std::ws;
          if (token == "SyzygyPath" && syzygy_tb_) {
            std::ostringstream oss;
            oss << "setoption name SyzygyPath value " << syzygy_tb_->get_paths();
            LOGFILE << oss.str();
            *vector_of_opstreams[our_index] << oss.str() << std::endl;	    
          }
        }
      }
    }
    vector_of_auxengine_ready_[our_index] = true;

    if(our_index == 0){
      // Initiate some stats and parameters (Threshold needs to be set
      // earlier, see search() in search.cc)

      // // Since we take a lock below, have to check if search is stopped.
      // if (stop_.load(std::memory_order_acquire)){
      // 	auxengine_mutex_.unlock();
      // 	return;
      // }

      search_stats_->AuxEngineTime = params_.GetAuxEngineTime();
      search_stats_->Number_of_nodes_added_by_AuxEngine = 0;
      search_stats_->Total_number_of_nodes = 0;
      if(search_stats_->New_Game){
	search_stats_->New_Game = false;
      }
    }
    
  } else {

    // AuxEngine(s) were already started. If we are thread zero then (1) Purge the queue(s) and (2) kickstart root if the queue is empty.

    if(our_index == 0){

      if(search_stats_->New_Game){
	search_stats_->AuxEngineTime = params_.GetAuxEngineTime();
	search_stats_->AuxEngineThreshold = params_.GetAuxEngineThreshold();
	search_stats_->Total_number_of_nodes = 0;
	search_stats_->Number_of_nodes_added_by_AuxEngine = 0;
	search_stats_->size_of_queue_at_start = 0;      

	// Occasionally, we get a new pointer to search_stats_ between games (not sure when/why that happens). When it happens, make sure the queues are empty, or the purging of them can fail.
	// Normally, everything works fine without the next four lines.
	search_stats_->persistent_queue_of_nodes = {}; 
	search_stats_->nodes_added_by_the_helper = {};
	search_stats_->source_of_PVs = {};
	search_stats_->source_of_queued_nodes = {};
	search_stats_->source_of_added_nodes = {};
	
	if (params_.GetAuxEngineVerbosity() >= 2) LOGFILE << "Resetting AuxEngine parameters because a new game started.";
	search_stats_->New_Game = false;
      }
    
      // purge obsolete nodes in the queue, if any. The even elements are the actual nodes, the odd elements is root if the preceding even element is still a relevant node.
      if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "search_stats_->size_of_queue_at_start:" << search_stats_->size_of_queue_at_start;
      if(search_stats_->final_purge_run){
	if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "Either we are not the first thread, or there is an unexpected order of execution, and final purging has already taken place. In either case not purging now.";
      } else {
	if(search_stats_->size_of_queue_at_start > 0){
	  int number_of_nodes_before_purging = int(search_stats_->size_of_queue_at_start / 2);
	  std::queue<Node*> persistent_queue_of_nodes_temp_;
	  for(int i=0; i < search_stats_->size_of_queue_at_start; i = i + 2){
	    Node * n = search_stats_->persistent_queue_of_nodes.front();
	    search_stats_->persistent_queue_of_nodes.pop();
	    Node * n_parent = search_stats_->persistent_queue_of_nodes.front();
	    search_stats_->persistent_queue_of_nodes.pop();
	    if(n_parent == root_node_){
	      // node is still relevant
	      persistent_queue_of_nodes_temp_.push(n);
	    }
	  }
	  // update search_stats_->persistent_queue_of_nodes
	  int my_size = persistent_queue_of_nodes_temp_.size();
	  for(int i=0; i < my_size; i++){      
	    search_stats_->persistent_queue_of_nodes.push(persistent_queue_of_nodes_temp_.front());
	    persistent_queue_of_nodes_temp_.pop();
	  }
	  if (params_.GetAuxEngineVerbosity() >= 4)
	    LOGFILE << "Purged " << number_of_nodes_before_purging - search_stats_->persistent_queue_of_nodes.size()
		    << " nodes from the query queue due to the move selected by the opponent. " << search_stats_->persistent_queue_of_nodes.size()
		    << " nodes remain in the queue.";
	}
	
	// Also purge stale nodes from the _added_ queue.
	if(search_stats_->nodes_added_by_the_helper.size() > 0){
	  int number_of_nodes_before_purging = int(search_stats_->nodes_added_by_the_helper.size() / 2);
	  std::queue<Node*> nodes_added_by_the_helper_temp_;
	  long unsigned int my_size = search_stats_->nodes_added_by_the_helper.size();      
	  for(long unsigned int i=0; i < my_size; i = i + 2){
	    Node * n = search_stats_->nodes_added_by_the_helper.front();
	    search_stats_->nodes_added_by_the_helper.pop();
	    Node * n_parent = search_stats_->nodes_added_by_the_helper.front();
	    search_stats_->nodes_added_by_the_helper.pop();
	    if(n_parent == root_node_){
	      // node is still relevant
	      nodes_added_by_the_helper_temp_.push(n);
	    }
	  }
	  // update search_stats_->nodes_added_by_the_helper
	  my_size = nodes_added_by_the_helper_temp_.size();
	  for(long unsigned int i=0; i < my_size; i++){      
	    search_stats_->nodes_added_by_the_helper.push(nodes_added_by_the_helper_temp_.front());
	    nodes_added_by_the_helper_temp_.pop();
	  }
	  if (params_.GetAuxEngineVerbosity() >= 4)
	    LOGFILE << "Purged " << number_of_nodes_before_purging - search_stats_->nodes_added_by_the_helper.size()
		    << " stale nodes from the queue of nodes added by the auxillary helper due to the move seleted by the opponent. " << search_stats_->nodes_added_by_the_helper.size()
		    << " nodes remain in the queue of nodes added by the auxillary helper.";
	}
      }

      // More stuff for thread zero only
      if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "AuxEngineWorker() finished purging/initiating, will now check if root can be queued";
      search_stats_->final_purge_run; // Inform other threads that they should not purge.
  
    } // Thread zero

    // This vector does not survive between moves, so it has to be
    // instantiated every move.
    auxengine_stopped_mutex_.lock();  
    auxengine_stopped_.push_back(true);
    auxengine_stopped_mutex_.unlock();      
    
  } // Not starting from scratch

  // now that auxengine_stopped_[x] is initiated, we can safely increase the thread_counter.
  search_stats_->thread_counter++;
    
  auxengine_mutex_.unlock();
  
  Node* n;
  bool not_yet_notified = true;
  while (!stop_.load(std::memory_order_acquire)) {
    // if we are thread zero, don't read from the queue, just take the root node.
    if(our_index == 0){
      // kickstart with the root node, no need to wait for it to get some
      // amount of visits. Except if root is not yet expanded, or lacks
      // edges for any other reason (e.g. being terminal), in which case
      // we should wait and try again later.
      if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "AuxEngineWorker() thread 0 about to aquire a shared lock nodes_mutex_ in order to read root";
      nodes_mutex_.lock_shared(); // only neede to read GetNumEdges(), SetAuxEngineMove(0xfffe) is already protected by auxengine_mutex_.lock();
      if(root_node_->GetNumEdges() > 0){
	// root is extended.
	root_node_->SetAuxEngineMove(0xfffe); // mark root as pending and queue it
	nodes_mutex_.unlock_shared(); // unlock the read-lock on noodes.
	auxengine_mutex_.lock();
    	search_stats_->source_of_queued_nodes.push(3); // inform DoAuxEngine() -> where this node came from.
	auxengine_mutex_.unlock(); // We will be in DoAuxEngine() until search is stopped, so unlock first.
	DoAuxEngine(root_node_, our_index);
      } else {
	nodes_mutex_.unlock_shared(); // unlock, nothing more to do until root gets edges.
	if (params_.GetAuxEngineVerbosity() >= 1) LOGFILE << "AuxEngineWorker() thread 0 found root node has no edges will sleep 100 ms";
	using namespace std::chrono_literals;
	std::this_thread::sleep_for(100ms);
      }
    } else {
      // Not thread 0
      if (not_yet_notified &&
	  params_.GetAuxEngineVerbosity() >= 5){
	LOGFILE << "AuxEngineWorker() thread: " << our_index << " entered main loop.";
	not_yet_notified = false;
      }
      {
	std::unique_lock<std::mutex> lock(auxengine_mutex_);
	// Wait until there's some work to compute.
	auxengine_cv_.wait(lock, [&] { return stop_.load(std::memory_order_acquire) || !search_stats_->persistent_queue_of_nodes.empty(); });
	if (stop_.load(std::memory_order_acquire)) {
	  auxengine_mutex_.unlock();
	  if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "AuxWorker(), thread " << our_index << " caught a stop signal, will exit the while loop now.";
	  break;
	}
	n = search_stats_->persistent_queue_of_nodes.front();
	search_stats_->persistent_queue_of_nodes.pop();
      } // release the lock
      DoAuxEngine(n, our_index);    
    } // end of not thread zero
  } // end of while loop

  // Decrement the thread counter so that purge in search.cc does not start before all threads are done.
  auxengine_mutex_.lock();
  search_stats_->thread_counter--;
  if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "AuxEngineWorker thread " << our_index << " done. The thread counter is now " << search_stats_->thread_counter;
  auxengine_mutex_.unlock();
}

  void Search::AuxEncode_and_Enqueue(std::string pv_as_string, int depth, ChessBoard my_board, Position my_position, std::vector<lczero::Move> my_moves_from_the_white_side, int source, bool require_some_depth) {
  // Take a string recieved from a helper engine, turn it into a vector with elements of type Move and queue that vector.

  // Quit early if search has stopped
 if(stop_.load(std::memory_order_acquire)) {
   if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "Would have quit early from AuxEncode_and_Enqueue() since search has stopped, but decided to take the risk and go on.";   
   // if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "Quitting early from AuxEncode_and_Enqueue() since search has stopped.";
   // return;
 }

  std::istringstream iss(pv_as_string);  
  std::string pv;
  std::vector<uint16_t> pv_moves;

  std::string s = "position fen " + GetFen(my_position); // for informational purposes only.
  std::string token;

  // To get the moves in UCI format, we have to construct a board, starting from root and then apply the moves.
  // Traverse up to root, and store the moves in a vector.
  // When we internally use the moves to extend nodes in the search tree, always use move as seen from the white side.
  // Apply the moves in reversed order to get the proper board state from which we can then make moves in legacy format.
  // std::vector<lczero::Move> my_moves;
  // std::vector<lczero::Move> my_moves_from_the_white_side;  
    
  bool flip = played_history_.IsBlackToMove() ^ (depth % 2 == 0);

  // auto bestmove_packed_int = Move(token, !flip).as_packed_int();
  // depth is distance between root and the starting point for the
  // auxengine.
  // depth_reached records the depth the helper claim to have search.
  // The PV is capped at this length (and can be shortened again in PreExt..()

  int pv_length = 1;
  int depth_reached = 0;
  int max_pv_length = 99; // Dirty work around for too many levels of recursion.

  while(iss >> pv >> std::ws) {
    if (pv == "info"){
      continue;
    }
    if (pv == "string"){
      // not for us.
      return;
    }
    if (pv == "depth") {
      // Figure out which depth was reached (can be zero).
      iss >> depth_reached >> std::ws;
      // if (params_.GetAuxEngineVerbosity() >= 9) LOGFILE << "Reached depth: " << depth_reached << " for node with depth: " << depth;
    }

    // Either "don't require depth" or depth > 14
    if (pv == "pv" && (! require_some_depth || depth_reached > 14)) {
      while(iss >> pv >> std::ws &&
	    pv_length < depth_reached &&
	    pv_length < max_pv_length) {
	Move m;
	if (!Move::ParseMove(&m, pv, !flip)) {	
	  if (params_.GetAuxEngineVerbosity() >= 1) LOGFILE << "Ignoring bad pv move: " << pv;
	  break;
	  // why not return instead of break?
	}
	// convert to Modern encoding, update the board and the position
	// For the conversion never flip the board. Only flip the board when you need to apply the move!
	Move m_in_modern_encoding = my_board.GetModernMove(m);

	if (my_board.flipped()) m_in_modern_encoding.Mirror();
	// Should the move applied be modern or legacy, or does it not matter?
	m_in_modern_encoding = my_board.GetModernMove(m_in_modern_encoding);
	// my_board.ApplyMove(m_in_modern_encoding); // Todo verify the correctness here, e.g. by printing a FEN.
	my_board.ApplyMove(m); // Todo verify the correctness here, e.g. by printing a FEN.	
	my_position = Position(my_position, m_in_modern_encoding);

	if (my_board.flipped()) m_in_modern_encoding.Mirror();
	my_board.Mirror();

	// my_moves.push_back(m); // Add the PV to the queue
	my_moves_from_the_white_side.push_back(m_in_modern_encoding); // Add the PV to the queue	
	pv_moves.push_back(m_in_modern_encoding.as_packed_int());
	flip = !flip;
	pv_length++;
      }
    }
  }

  if (pv_moves.size() > 0){
    if (params_.GetAuxEngineVerbosity() >= 9){
      std::string debug_string;
      // No lock required here, my_moves_from_the_white_side is only a simple queue of Moves, it has nothing to do with the searchtree.
      for(int i = 0; i < (int) my_moves_from_the_white_side.size(); i++){
	debug_string = debug_string + my_moves_from_the_white_side[i].as_string() + " ";
      }
      if(played_history_.IsBlackToMove()){
	LOGFILE << "debug info: length of PV given to helper engine: " << depth << " position given to helper: " << s << " black to move at root, length of my_moves_from_the_white_side " << my_moves_from_the_white_side.size() << " my_moves_from_the_white_side: " << debug_string;
      } else {
	LOGFILE << "debug info: length of PV given to helper engine: " << depth << " position given to helper: " << s << " white to move at root, length of my_moves_from_the_white_side " << my_moves_from_the_white_side.size() << " my_moves_from_the_white_side: " << debug_string;
      }
    }
    fast_track_extend_and_evaluate_queue_mutex_.lock(); // lock this queue before starting to modify it
    fast_track_extend_and_evaluate_queue_.push(my_moves_from_the_white_side);
    fast_track_extend_and_evaluate_queue_mutex_.unlock();
    auxengine_mutex_.lock();
    search_stats_->source_of_PVs.push(source);
    auxengine_mutex_.unlock();
  }

}

void Search::DoAuxEngine(Node* n, int index){
  // before trying to take a lock on nodes_mutex_, always check if search has stopped, in which case we return early
  if(stop_.load(std::memory_order_acquire)) {
    if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "DoAuxEngine, thread " << index << " caught a stop signal beforing doing anything.";
    return;
  }

  if (params_.GetAuxEngineVerbosity() >= 9){
    nodes_mutex_.lock_shared();
    LOGFILE << "DoAuxEngine() called for node" << n->DebugString() << " thread: " << index;
    nodes_mutex_.unlock_shared();
  }

  // Calculate depth.
  int depth = 0;
  if(n != root_node_){
    if(stop_.load(std::memory_order_acquire)) {
      if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "DoAuxEngine caught a stop signal before starting to calculate depth.";
      return;
    }
    nodes_mutex_.lock_shared();
    for (Node* n2 = n; n2 != root_node_; n2 = n2->GetParent()) {
      depth++;
    }
    nodes_mutex_.unlock_shared();    
  }
  // // Since we take a lock below, have to check if search is stopped.
  // if (stop_.load(std::memory_order_acquire)){
  //   return;
  // }
  // if there is no node in the queue then accept unconditionally.
  auxengine_mutex_.lock();
  if (search_stats_->persistent_queue_of_nodes.size() > 0){
    float sample = distribution(generator);
    if(depth > 0 &&
       depth > params_.GetAuxEngineMaxDepth() &&
       float(1.0f)/(depth) < sample){
      // This is exactly what SearchWorker::AuxMaybeEnqueueNode() does, but we are in class Search:: now, so that function is not available.
      int source = search_stats_->source_of_queued_nodes.front();
      search_stats_->source_of_queued_nodes.pop();
      search_stats_->persistent_queue_of_nodes.push(n);
      search_stats_->source_of_queued_nodes.push(source);
      auxengine_cv_.notify_one(); // unnecessary?
      auxengine_mutex_.unlock();
      return;
    }
  }
  auxengine_mutex_.unlock();  
  
  if(depth > 0 &&
     depth > params_.GetAuxEngineMaxDepth()){
    // if (params_.GetAuxEngineVerbosity() >= 6) LOGFILE << "DoAuxEngine processing a node with high depth: " << " since sample " << sample << " is less than " << float(1.0f)/(depth);
  }
    
  // if (params_.GetAuxEngineVerbosity() >= 6) LOGFILE << "DoAuxEngine processing a node with depth: " << depth;

  std::string s = "";
  bool flip = played_history_.IsBlackToMove() ^ (depth % 2 == 0);

  // To get the moves in UCI format, we have to construct a board, starting from root and then apply the moves.
  // Traverse up to root, and store the moves in a vector.
  // When we internally use the moves to extend nodes in the search tree, always use move as seen from the white side.
  // Apply the moves in reversed order to get the proper board state from which we can then make moves in legacy format.
  std::vector<lczero::Move> my_moves;
  std::vector<lczero::Move> my_moves_from_the_white_side;  

  if(n != root_node_){
    if(stop_.load(std::memory_order_acquire)) {
      if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "DoAuxEngine caught a stop signal while populating my_moves.";
      return;
    }
    nodes_mutex_.lock_shared();  
    for (Node* n2 = n; n2 != root_node_; n2 = n2->GetParent()) {
      my_moves.push_back(n2->GetOwnEdge()->GetMove(flip));
      my_moves_from_the_white_side.push_back(n2->GetOwnEdge()->GetMove());
      flip = !flip;
    }
    nodes_mutex_.unlock_shared();
  }

  // Reverse the order
  std::reverse(my_moves.begin(), my_moves.end());
  std::reverse(my_moves_from_the_white_side.begin(), my_moves_from_the_white_side.end());
    
  ChessBoard my_board = played_history_.Last().GetBoard();
  Position my_position = played_history_.Last();

  // modern encoding
  for(auto& move: my_moves) {
    if (my_board.flipped()) move.Mirror();
    my_board.ApplyMove(move);
    my_position = Position(my_position, move);
    if (my_board.flipped()) move.Mirror();
    s = s + move.as_string() + " ";  // only for debugging
    my_board.Mirror();
  }

  if (params_.GetAuxEngineVerbosity() >= 9) LOGFILE << "add pv=" << s << " from root position: " << GetFen(played_history_.Last());
  s = "position fen " + GetFen(my_position);
  
  // Before starting, test if stop_ is set
  if (stop_.load(std::memory_order_acquire)) {
    if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "DoAuxEngine caught a stop signal 1.";
    return;
  }

  auto auxengine_start_time = std::chrono::steady_clock::now();
  // auxengine_os_ << s << std::endl;
  *vector_of_opstreams[index] << s << std::endl;  

  if(index == 0){
    if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "Starting infinite query from root node for thread 0";
    *vector_of_opstreams[index] << "go infinite " << std::endl;
  } else {
    // This is a read of search_stats_ without a lock, is that OK?
    *vector_of_opstreams[index] << "go movetime " << search_stats_->AuxEngineTime << std::endl;
  }

  auxengine_stopped_mutex_.lock();
  if(auxengine_stopped_[index]){
    if (params_.GetAuxEngineVerbosity() >= 10) LOGFILE << "Setting auxengine_stopped_ to false for thread " << index;
    auxengine_stopped_[index] = false;    
  }
  auxengine_stopped_mutex_.unlock();

  std::string prev_line;
  std::string my_line;
  std::string line;
  std::string token;
  std::string my_token;  
  bool stopping = false;
  bool second_stopping = false;  
  // while(std::getline(auxengine_is_, line)) {  
  while(std::getline(*vector_of_ipstreams[index], line)) {
    if (params_.GetAuxEngineVerbosity() >= 9) {
      LOGFILE << "thread: " << index << " auxe:" << line;
    }

    std::istringstream iss(line);
    iss >> token >> std::ws;

    if (token == "bestmove") {
      iss >> token;
      if(token == "info"){
	if (params_.GetAuxEngineVerbosity() >= 1) LOGFILE << "Hit a case of https://github.com/hans-ekbrand/lc0/issues/9";
	// This is a case of https://github.com/hans-ekbrand/lc0/issues/9
	// bestmove:info" indicates something is corrupted in the input stream.
	// issue `stop`, stay in the loop and try another iteration.
	// TODO: If the next iteration also fails, stop and restart the engine.
	*vector_of_opstreams[index] << "stop" << std::endl;
      } else {
	break;
      }
    }
    prev_line = line;

    // Don't send a second stop command
    if (!stopping) {
      stopping = stop_.load(std::memory_order_acquire);
      if (stopping) {
	if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "DoAuxEngine(), thread=" << index << " caught a stop signal 2.";	
        // (unless someone else already has sent stop) send stop,
	// stay in loop to get best response, otherwise it
        // will disturb the next iteration.
	// only send stop if we are the first to detect that search has stopped.
	auxengine_stopped_mutex_.lock();
	if(!auxengine_stopped_[index]){
	  if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "DoAuxEngine(), thread=" << index << " Stopping the A/B helper Start";
	  *vector_of_opstreams[index] << "stop" << std::endl; // stop the A/B helper	  
	  if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "DoAuxEngine(), thread=" << index << " Stopping the A/B helper Stop";
	  auxengine_stopped_[index] = true;
	}
	auxengine_stopped_mutex_.unlock();
      } else {
	// Since we are not stopping, do the ordinary stuff
	// parse and queue PV:s even before the search is finished, if the depth is high enough (which will be determined by AuxEncode_and_Enqueue().
	if (token == "info") {
	  // Since we (possibly) now create multiple PV:s per node, also (possibly) add a source.
	  auxengine_mutex_.lock(); // take a lock even if we are just reading
	  int source = search_stats_->source_of_queued_nodes.front();
	  auxengine_mutex_.unlock();
	  AuxEncode_and_Enqueue(line, depth, my_board, my_position, my_moves_from_the_white_side, source, true);
	}
      }
    } else {
      if(!second_stopping){
	if (params_.GetAuxEngineVerbosity() >= 1) LOGFILE << "We found that search is stopped, but the next line from the helper was not 'bestmove'. Weird! As a workaround send yet another stop";
	// We were stopping already before going into this iteration, but the helper did not respond "bestmove", as it ought to have done. Send stop again
	if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "DoAuxEngine(), thread=" << index << " Stopping for the second time the A/B helper Start";
	*vector_of_opstreams[index] << "stop" << std::endl; // stop the A/B helper	  
	if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "DoAuxEngine(), thread=" << index << " Stopping for the second time the A/B helper Stop";
	second_stopping = true;
      }
    }
  }
  if (stopping) {
    // Don't use results of a search that was stopped.
    // Not because the are unreliable, but we simply want to shut down as fast as possible.
    return;
  }
  auxengine_stopped_mutex_.lock();
  auxengine_stopped_[index] = true; // stopped means "not running". It does not mean it was stopped prematurely.
  auxengine_stopped_mutex_.unlock();
  
  if (params_.GetAuxEngineVerbosity() >= 9) {
    LOGFILE << "pv:" << prev_line;
    LOGFILE << "bestanswer:" << token;
  }
  if(prev_line == ""){
    if (params_.GetAuxEngineVerbosity() >= 1) LOGFILE << "Empty PV, returning early from doAuxEngine().";
    return;
  }
  if (! vector_of_children[index]->running()) {
    LOGFILE << "AuxEngine died!";
    throw Exception("AuxEngine died!");
  }
  auto auxengine_dur =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - auxengine_start_time)
      .count();
  auxengine_total_dur += auxengine_dur;
  auxengine_num_evals++;
  
  auxengine_mutex_.lock();
  int source = search_stats_->source_of_queued_nodes.front();
  search_stats_->source_of_queued_nodes.pop();
  auxengine_mutex_.unlock();
  
  AuxEncode_and_Enqueue(prev_line, depth, my_board, my_position, my_moves_from_the_white_side, source, false);  

}

void Search::AuxWait() {  
  while (!auxengine_threads_.empty()) {
    Mutex::Lock lock(threads_mutex_);
    if (params_.GetAuxEngineVerbosity() >= 7) LOGFILE << "AuxWait about to pop AuxEngineWorker() threads";
    auxengine_threads_.back().join();
    auxengine_threads_.pop_back();
  }

  auxengine_mutex_.lock();  
  search_stats_->Number_of_nodes_added_by_AuxEngine = search_stats_->Number_of_nodes_added_by_AuxEngine + auxengine_num_updates;
  float observed_ratio = float(search_stats_->Number_of_nodes_added_by_AuxEngine) / search_stats_->Total_number_of_nodes;

  // Decrease the EngineTime if we're in an endgame.
  ChessBoard my_board = played_history_.Last().GetBoard();
  if((my_board.ours() | my_board.theirs()).count() < 20){
    search_stats_->AuxEngineTime = std::max(10, int(std::round(params_.GetAuxEngineTime() * 0.50f))); // minimum 10 ms.
  }

  // Time based queries    
  if (params_.GetAuxEngineVerbosity() >= 3) LOGFILE << "Summaries per move: (Time based queries) persistent_queue_of_nodes size at the end of search: " << search_stats_->AuxEngineQueueSizeAtMoveSelectionTime
	  << " Ratio added/total nodes: " << observed_ratio << " (added=" << search_stats_->Number_of_nodes_added_by_AuxEngine << "; total=" << search_stats_->Total_number_of_nodes << ")."
      << " Average duration " << (auxengine_num_evals ? (auxengine_total_dur / auxengine_num_evals) : -1.0f) << "ms"
      << " AuxEngineTime for next iteration " << search_stats_->AuxEngineTime
      << " New AuxEngineThreshold for next iteration " << search_stats_->AuxEngineThreshold
      << " Number of evals " << auxengine_num_evals
      << " Number of added nodes " << search_stats_->Number_of_nodes_added_by_AuxEngine;

  // Reset counters for the next move:
  search_stats_->Number_of_nodes_added_by_AuxEngine = 0;
  search_stats_->Total_number_of_nodes = 0;

  auxengine_mutex_.unlock();  
  
  // Empty the other queue.
  fast_track_extend_and_evaluate_queue_mutex_.lock();
  if(fast_track_extend_and_evaluate_queue_.empty()){
    if (params_.GetAuxEngineVerbosity() >= 4) LOGFILE << "No PVs in the fast_track_extend_and_evaluate_queue";
  } else {
    if (params_.GetAuxEngineVerbosity() >= 4) LOGFILE << fast_track_extend_and_evaluate_queue_.size() << " possibly obsolete PV:s in the queue.";
    fast_track_extend_and_evaluate_queue_ = {};
    // Also empty the source queue
    search_stats_->source_of_PVs = {};    
    // TODO save the PV if it is still relevant
    if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "Number of PV:s in the queue=" << fast_track_extend_and_evaluate_queue_.size();
  }
  fast_track_extend_and_evaluate_queue_mutex_.unlock();  
  if (params_.GetAuxEngineVerbosity() >= 5) LOGFILE << "AuxWait done search_stats_ at: " << &search_stats_;
}

}  // namespace lczero
