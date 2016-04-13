#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <time.h>

#include "bitboards.h"
#include "bitutils.h"
#include "board.h"
#include "castle.h"
#include "evaluate.h"
#include "magics.h"
#include "piece.h"
#include "psqt.h"
#include "search.h"
#include "transposition.h"
#include "types.h"
#include "move.h"
#include "movegen.h"
#include "movegentest.h"
#include "zorbist.h"


int USE_NULL_MOVE_PRUNING            = 1;
int USE_INTERNAL_ITERATIVE_DEEPENING = 1;
int USE_FUTILITY_PRUNING             = 1;
int USE_LATE_MOVE_REDUCTIONS         = 1;

int IID_FAILURE;
int IID_SUCCESS;
int IID_NWASTED;

int NMP_FAILURE;
int NMP_SUCCESS;
int NMP_NWASTED;

uint16_t BestMove;

time_t StartTime;
time_t EndTime;

long NodesSearched;
int EvaluatingPlayer;

uint16_t KillerMoves[MaxHeight][2];
uint16_t KillerCaptures[MaxHeight][2];

TranspositionTable Table;

uint16_t get_best_move(Board * board, int seconds, int send_results){
    
    IID_FAILURE = 0;
    IID_SUCCESS = 0;
    IID_NWASTED = 0;
    
    int value, depth, i, size=0;
    uint16_t PV[MaxHeight];
    
    StartTime = time(NULL);
    EndTime = StartTime + seconds;
    
    NodesSearched = 0;
    EvaluatingPlayer = board->turn;
    
    init_transposition_table(&Table, 24);
    
    printf("Starting Search.....\n");
    print_board(board);
    printf("\n\n");
    printf("<-----------------SEARCH RESULTS----------------->\n");
    printf("|  Depth  |  Score  |   Nodes   | Elapsed | Best |\n");
    
    for (depth = 1; depth < MaxDepth; depth++){
        value = alpha_beta_prune(board,-Mate,Mate,depth,0,PVNODE);
        printf("|%9d|%9d|%11d|%9d| ",depth,value,NodesSearched,(time(NULL)-StartTime));
        print_move(BestMove);
        printf(" |\n");
        
        if (send_results){
            printf("info depth %d score cp %d time %d pv ",depth,value,time(NULL)-StartTime);
            print_move(BestMove);
            printf("\n");
        }
        
        if (time(NULL) - StartTime > seconds)
            break;
        
        if (value > evaluate_board(board)){
			if ((time(NULL) - StartTime) * 4 > seconds)
				break;
		}
		else {
			if ((time(NULL) - StartTime) * 3 > seconds)
				break;
		}
    }
    
    dump_transposition_table(&Table);
    
    printf("SUCCESS IID %d\n",IID_SUCCESS);
    printf("FAILURE IID %d\n",IID_FAILURE);
    printf("NWASTED IID %d\n",IID_NWASTED);
    
    printf("FAILURE NMP %d\n",NMP_FAILURE);
    printf("SUCCESS NMP %d\n",NMP_SUCCESS);
    printf("NWASTED NMP %d\n",NMP_NWASTED);


    return BestMove;
}

int alpha_beta_prune(Board * board, int alpha, int beta, int depth, int height, int node_type){
    int i, value, valid = 0, size = 0, best = -2*Mate;
    int in_check, opt_value;
    int initial_alpha = alpha;
    int table_turn_matches = 0;
    int used_table_entry = 0;
    uint16_t table_move, best_move, moves[256];
    Undo undo[1];
    
    // Alloted Time has Expired
    if (EndTime < time(NULL))
        return board->turn == EvaluatingPlayer ? -Mate : Mate;
    
    // Max Depth Reached
    if (depth == 0)
        return quiescence_search(board,alpha,beta,height);
    
    // Max Height Reached
    if (height >= MaxHeight)
        return quiescence_search(board,alpha,beta,height);
    
    // Updated Node Counter
    NodesSearched += 1;
    
    // Perform Transposition Table Lookup
    TranspositionEntry * entry = get_transposition_entry(&Table, board->hash);
    if (entry != NULL){
        table_move = entry->best_move;
        
        if (board->turn == entry->turn)
            table_turn_matches = 1;
        
        if (entry->depth >= depth && board->turn == entry->turn){
            if (entry->type == PVNODE)
                return entry->value;
            else if (entry->type == CUTNODE && entry->value > alpha)
                alpha = entry->value;
            else if (entry->type == ALLNODE && entry->value < beta)
                beta = entry->value;
            
            if (alpha >= beta)
                return entry->value;
            
            used_table_entry = 1;
        }       
    }
    
    // Determine 3-Fold Repetition
    int reps = 0;
    for (i = 0; i < board->move_num; i++)
        if (board->history[i] == board->hash)
            reps += 1;    
    if (reps >= 2)
        return 0;
    
    // Null Move Pruning
    if (USE_NULL_MOVE_PRUNING){
		if (depth > 3 &&
			evaluate_board(board) >= beta &&
			is_not_in_check(board,board->turn)){
				
			int temp = NodesSearched;
				
			board->turn = !board->turn;
			board->history[board->move_num++] = NoneMove;
			int eval = -alpha_beta_prune(board,-beta,-beta+1,depth-3,height,ALLNODE);
			board->move_num -= 1;
			board->turn = !board->turn;
				
			if (eval >= beta){
				NMP_SUCCESS++;
				return eval;
			}
			
			NMP_FAILURE++;
			NMP_NWASTED += NodesSearched - temp;
		}
	}
	
    // Internal Iterative Deepening
    if (USE_INTERNAL_ITERATIVE_DEEPENING){
		if (depth >= 3 && !table_turn_matches){
            int temp = NodesSearched;
			value = alpha_beta_prune(board,alpha,beta,depth-3,height,node_type);
			if (value <= alpha)
				value = alpha_beta_prune(board,-Mate,beta,depth-3,height,node_type);
			
			TranspositionEntry * entry = get_transposition_entry(&Table, board->hash);
			if (entry != NULL){
                IID_SUCCESS++;
				table_move = entry->best_move;
            } else {
                IID_NWASTED += NodesSearched - temp;
                IID_FAILURE++;
            }
		}
	}
    
    // Generate Moves & Sort Moves
    gen_all_moves(board,moves,&size);
    sort_moves(board,moves,size,depth,height,table_move);
    in_check = !is_not_in_check(board,board->turn);
    opt_value = Mate;
    
    for (i = 0; i < size; i++){
        
        //Futility Pruning
        if (USE_FUTILITY_PRUNING){
			if (valid >= 1 && depth == 1 && !in_check && MOVE_TYPE(moves[i]) == NormalMove){
				if (board->squares[MOVE_TO(moves[i])] == Empty){
					if (opt_value == Mate)
						opt_value = evaluate_board(board) + PawnValue;
					
					value = opt_value;
					
					if (value <= alpha){
						continue;
					}
				}
			}
		}
        
        apply_move(board,moves[i],undo);
        
        // Ensure move is Legal
        if (!is_not_in_check(board,!board->turn)){
            revert_move(board,moves[i],undo);
            continue;
        }
        
        valid++;
        
        // Principle Variation Search
        if (valid == 1)
            value = -alpha_beta_prune(board,-beta,-alpha,depth-1,height+1,PVNODE);
        else {
			if (USE_LATE_MOVE_REDUCTIONS){
				if (table_turn_matches && 
                    valid > 8 && 
					depth >= 4 && 
					!in_check && 
					MOVE_TYPE(moves[i]) == NormalMove &&
					undo[0].capture_piece == Empty)
					
					value = -alpha_beta_prune(board,-alpha-1,-alpha,depth-2,height+1,CUTNODE);
				else
					value = -alpha_beta_prune(board,-alpha-1,-alpha,depth-1,height+1,CUTNODE);
			} else {
				value = -alpha_beta_prune(board,-alpha-1,-alpha,depth-1,height+1,CUTNODE);
			}
            
            if (value > alpha)
                value = -alpha_beta_prune(board,-beta,-alpha,depth-1,height+1,PVNODE);
        }
        
        revert_move(board,moves[i],undo);
        
        // Update Search Bounds
        if (value > best){              
            best = value;
            best_move = moves[i];
    
            if (best > alpha){
                alpha = best;   
            }
        }
        
        if (alpha >= beta){
            if (undo->capture_piece == Empty || MOVE_TYPE(moves[i]) != NormalMove){
                KillerMoves[height][1] = KillerMoves[height][0];
                KillerMoves[height][0] = moves[i];
            } else {
                KillerCaptures[height][1] = KillerCaptures[height][0];
                KillerCaptures[height][0] = moves[i];
            }
        
            break;
        }
    }
    
        
    // Check for Stalemate and CheckMate
    if (valid == 0){
        if (is_not_in_check(board,board->turn)){
            store_transposition_entry(&Table, MaxDepth-1, board->turn, PVNODE, 0, NoneMove, board->hash);
            return 0;
        } else {
            store_transposition_entry(&Table, MaxDepth-1, board->turn, PVNODE, -Mate+height, NoneMove, board->hash);
            return -Mate+height;
        }
    }
    
    // Store in Transposition Table    
    if (best > initial_alpha && best < beta)
        store_transposition_entry(&Table, depth, board->turn,  PVNODE, best, best_move, board->hash);
    else {
        if (!used_table_entry){
            if (best >= beta)
                store_transposition_entry(&Table, depth, board->turn, CUTNODE, best, best_move, board->hash);
            else if (best <= initial_alpha)
                store_transposition_entry(&Table, depth, board->turn, ALLNODE, best, best_move, board->hash);
        }
    }

    
    if (height == 0)
        BestMove = best_move;
    
    return best;
}

int quiescence_search(Board * board, int alpha, int beta, int height){
    if (height >= MaxHeight)
        return evaluate_board(board);
    
    int value = evaluate_board(board);
    
    if (value > alpha)
        alpha = value;
    
    if (alpha > beta)
        return value;
    
    NodesSearched += 1;
    
    Undo undo[1];
    int i, size = 0;
    uint16_t moves[256];
    
    int best = value;
    gen_all_non_quiet(board,moves,&size);
    sort_moves(board,moves,size,0,height,NoneMove);
    
    for(i = 0; i < size; i++){
        apply_move(board,moves[i],undo);
        
        if (!is_not_in_check(board,!board->turn)){
            revert_move(board,moves[i],undo);
            continue;
        }
        
        value = -quiescence_search(board,-beta,-alpha,height+1);
        
        revert_move(board,moves[i],undo);
        
        if (value > best)
            best = value;
        if (best > alpha)
            alpha = best;
        if (alpha > beta){
            KillerCaptures[height][1] = KillerCaptures[height][0];
            KillerCaptures[height][0] = moves[i];
            break;
        }
    }
    
    return best;
}

void sort_moves(Board * board, uint16_t * moves, int size, int depth, int height, uint16_t best_move){
    int values[size], value;
    int i, j;
    
    int temp_value;
    uint16_t temp_move;
    
    uint16_t killer1 = KillerMoves[height][0];
    uint16_t killer2 = KillerMoves[height][1];
    uint16_t killer3 = KillerCaptures[height][0];
    uint16_t killer4 = KillerCaptures[height][1];
    
    for (i = 0; i < size; i++){
        value  = 8192 * ( best_move == moves[i]);
        value +=  512 * (   killer1 == moves[i]);
        value +=  256 * (   killer2 == moves[i]);
        value +=  512 * (   killer3 == moves[i]);
        value +=  256 * (   killer4 == moves[i]);
        
        int to_val = PieceValues[PIECE_TYPE(board->squares[MOVE_TO(moves[i])])];
        int from_val = PieceValues[PIECE_TYPE(board->squares[MOVE_FROM(moves[i])])];
        
        value += 5 * to_val;
        value -= 1 * from_val;

        if (MOVE_TYPE(moves[i]) == EnpassMove)
            value += PawnValue;
        
        if (MOVE_TYPE(moves[i]) == PromotionMove)
            value += 32 << (MOVE_PROMO_TYPE(moves[i]) >> 14);
        
        values[i] = value;
    }
    
    for (i = 0; i < size; i++){
        for (j = i + 1; j < size; j++){
            if (values[j] > values[i]){
                temp_value = values[j];
                temp_move = moves[j];
                
                values[j] = values[i];
                moves[j] = moves[i];
                
                values[i] = temp_value;
                moves[i] = temp_move;
            }
        }
    }
}