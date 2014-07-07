#include "utils/System.h"
#include "core/pSolver.h"
#include "mtl/Sort.h"
#include <iostream>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <sstream>
using namespace Glucose;

/*RAJOUT STAGE*/


void __clean_(void *arg) {
  pthread_mutex_unlock((pthread_mutex_t*)arg); 
}

/*FIN RAJOUT STAGE*/


int pSolver::nbworkers=0;
pSolver ** pSolver::solvers=0;
int pSolver::folio=false;
Queue<Lit> pSolver::vars;

// This method executes the main procedure 
// of the resolution process:  a thread starts
// first in portfolio mode then switches   
// to divide and conquer mode. When it terminates 
// it goes in a spin loop waiting a cancellation
// signal from the pricipal process

/*RAJOUT STAGE*/


void* pSolver::thread_work() {
  if(!folio) 
    searchSubProblem();
  else
    getPromisingOrder();

  while(true) usleep(5000);

  return 0;
}

//The bootstrap function that have to be 
//executed by each process
void* pSolver::bootstrap(void * arg) {
  pSolver* solver = (pSolver *)arg;
  return solver->thread_work();
}

//Create a worker that will handle subproblems
pthread_t pSolver::create_worker(void *arg) {
  pthread_t tid;
  pthread_create(&tid, 0, &pSolver:: bootstrap, arg); 
  return tid;
}

//Replay a set of assignements 
//to position back the solver to 
//a certain point.
lbool pSolver::playAssumptions(){
    
  while (decisionLevel() < assumptions.size()){
    // Perform user provided assumption:
    Lit p = assumptions[decisionLevel()];
    if (value(p) == l_True){
      newDecisionLevel();
    }else if (value(p) == l_False){
      return l_False;  
    }else{
      newDecisionLevel();
      uncheckedEnqueue(p);
      if (propagate() != CRef_Undef){
	return l_False;
      }
    }
  }
  
  return l_True;
}


//Construct the starting points (literals)
//of the portfolio phase of the solving process
void pSolver::getfirstLits(){

  int dist=distance;

  random_var_freq=1;
  Lit   l=lit_Undef;
  do 
      l = pickBranchLit();
  while(watches[l].size() == 0);
  random_var_freq=0;

  for(int i=0; i <nbworkers-1;++i)
    vars.insert(l);

  while(dist != 0){
   
    for(int i=0; i <nbworkers-1;++i){
      Lit e = vars.peek(); vars.pop();
      vec<Watcher>&  ws  = watches[e];
      int cl= irand(random_seed, ws.size());
      Clause&  c         = ca[ws[cl].cref];

      Lit li= e;
      
      for(int k=0; k <c.size();++k){
	if( c[k] != e && 
	    watches[c[k]].size() == 0 &&
	    value(c[k]) != l_Undef){
	  li = c[k];
	  break;
	}
      }
      
      vars.insert(li);   
    }	
    dist--;
  }
}


//Portfolio phase: try to find 
//a solution or at least find a
//promising literals' order that
//can be exploited in the devide and conquer
// phase. 
void  pSolver::getPromisingOrder(){
  lbool ret;
  
  model.clear();
  conflict.clear();
  nbjobs = 0;

  if (!ok) return;

  sumLBD = 0;  
  lbdQueue.initSize(sizeLBDQueue);
  trailQueue.initSize(sizeTrailQueue);
  nbclausesbeforereduce = firstReduceDB;    
  cancelUntil(0);
  
  solves++;  

  
   Lit   next = lit_Undef;
  //Lit   next = vars[id -1];
  //const Lit lit_Undef = { -2 };

  nbjobs++;  
  
  //Perform a CDCL search process on the problem
  //Beside, learn new clauses from othern workers.  
  while( l_Undef == ( ret =  search(next)) ){
    pthread_testcancel();   
    //Update the DB of learned clauses by
    //those of other workers
    sharedBase->update(this);
    next =lit_Undef;
  }
    
  //The problem is SAT
  if (ret == l_True) {
    mpz_t tmp;  
    mpz_init(tmp);
    thtime += cpuThreadTime(); 
    mpz_set_si(tmp, 1);
    sharedJob->putResult(tmp);
    mpz_clear(tmp); 
    
  pthread_cleanup_push(__clean_, &mutex);
  pthread_mutex_lock(&mutex);

    solvers[0]->model.growTo(nVars());
    for (int i = 0; i < nVars(); i++) 
      solvers[0]->model[i] = value(i);

  pthread_mutex_unlock(&mutex);
  pthread_cleanup_pop(0);

    return;
  } 
 
  //The problem is UNSAT
  if (ret == l_False) {
    mpz_t tmp;  
    mpz_init(tmp);
    thtime += cpuThreadTime(); 
    mpz_set_si(tmp, -1);
    sharedJob->putResult(tmp);
    mpz_clear(tmp); 
    return;
  } 
 

}



//Tha main thread's work: pick a subproblem and try to solve 
//it. If it too hard try to decopmose it and dispach
//parts to idle workers. 
void pSolver::searchSubProblem() {
  lbool ret;
  
  model.clear();
  conflict.clear();
  nbjobs = 0;

  if (!ok) return;

  while (true) {
  start:
    sumLBD = 0; conflicts=0;
    lbdQueue.initSize(sizeLBDQueue);
    trailQueue.initSize(sizeTrailQueue);
    nbclausesbeforereduce = firstReduceDB;    
    cancelUntil(0);
        
    if (!sharedJob->pop(assumptions)) {
      mpz_t tmp;  
      mpz_init(tmp);
      mpz_set_si(tmp, -1);
      sharedJob->putResult(tmp);
      mpz_clear(tmp); 
      thtime += cpuThreadTime();
      break;
    }
    
    solves++;  

    //Decompose the subproblem if it
    //is the last in the queue
    if(nAssigns() <= percentage * nVars() &&
       sharedJob->getJobLeft() ==0){
      
      if(playAssumptions() == l_False){
	goto start;
      }
      else
	 createJobs(decisionLevel()+sharedJob->getHeight());
      continue;
    }
    
    nbjobs++;   

    //Perform a CDCL search process on the
    //subproblem while it is Undef: not known to be SAT or UNSAT.
    //Beside, learn new clauses from othern workers.  
    while( l_Undef == ( ret =  search(0)) ){
      
      pthread_testcancel();   

      //Update the DB of learned clauses by
      //those of other workers
      sharedBase->update(this);
            
      //If the subproblem is Undef and
      //the number of assigned variables is less
      //than a percentage, and there is some idle thread
      //then decompose the subproblem.
      if(nAssigns() <= percentage * nVars() &&
	 sharedJob->getJobLeft() ==0){
	if(playAssumptions() == l_False)
	  goto start;
	else
	   createJobs(decisionLevel()+sharedJob->getHeight());
	break;
      }
    }
    
    //The problem is SAT 
    if (ret == l_True) {
      mpz_t tmp;  
      mpz_init(tmp);
      thtime += cpuThreadTime(); 
      mpz_set_si(tmp, 1);
      sharedJob->putResult(tmp);
      mpz_clear(tmp);
      std::cout << "Solution Thread="<< id <<std::endl;

  pthread_cleanup_push(__clean_, &mutex);
  pthread_mutex_lock(&mutex);

      solvers[0]->model.growTo(nVars());
      for (int i = 0; i < nVars(); i++) 
	solvers[0]->model[i] = value(i);
      break;

  pthread_mutex_unlock(&mutex);
  pthread_cleanup_pop(0);

    }

    //The problem is UNSAT
    if (ret == l_False && decisionLevel() == 0) {
      mpz_t tmp;  
      mpz_init(tmp);
      thtime += cpuThreadTime(); 
      mpz_set_si(tmp, -1);
      sharedJob->putResult(tmp);
      mpz_clear(tmp); 
      break;
    } 
 
  }
}

//This procedure generates subproblems the 
//assumption of which have a size of h (the numbre of 
//decision levels is g. t. e. to h)
bool pSolver::createJobs(unsigned int h){
  assert(ok); 

 
  // Unit propagation
  CRef confl = propagate();

  if ((unsigned) confl != CRef_Undef){
    // CONFLICT : just store the learnt clause.
    int         backtrack_level;
    vec<Lit>    learnt_clause;
    unsigned int nblevels;      

    conflicts++; 	    
    learnt_clause.clear();
    analyze(confl, learnt_clause, backtrack_level,nblevels);
               
    if (learnt_clause.size() != 1){
      CRef cr = ca.alloc(learnt_clause, true);
      learnts.push(cr);

      /*RAJOUT STAGE*/

  if(ca[cr].setgel(to_freeze_var_act_clauses(cr, activity[order_heap[0]])) == 0){
    attachClause(cr);
    ca[cr].setused();
  }

  nb_clauses_total++;
  
//ca[cr].setgel(1);

/*
                Freeze_params fp = mkFreeze(0, to_freeze_var_act_clauses(cr,0));
                fp.freeze = 0;//to_freeze(cr);
                //frozen_clauses.push(fp);
*/
      /*FIN RAJOUT STAGE*/

      
      claBumpActivity(ca[cr]);
    }

    varDecayActivity();
    claDecayActivity();
    
    return false;

  }else{

    //The decion level 'h' is reached:
    // inqueue the new found job and return
    if(decisionLevel()>=(int)h){
      sharedJob->push(trail);
      return false;
    }

    decisions++;
    
    Lit next =  pickBranchLit();
   
    // If all vars are assigned 
    if (next == lit_Undef){
      std::cout << "Create Job Thread="<< id <<std::endl;
      return true;
    }
    // At least one var is not assigned, 
    // compute create a branch for next
    // and an other for !next
    else {
      newDecisionLevel();
      uncheckedEnqueue(next);
      bool res=false;
      if (!(res= createJobs(h))){
	cancelUntil(decisionLevel()-1);
	newDecisionLevel();
	uncheckedEnqueue(~next);
	res =  createJobs(h);
	cancelUntil(decisionLevel()-1);
      }
      return res;
    }
  }  
}

//The CDCL procedure is encoded here
lbool pSolver::search(int nof_conflicts)
{
  assert(ok);
  int          backtrack_level;
  int          conflictC = 0;
  vec<Lit>     learnt_clause;
  unsigned int nblevels;
  bool         blocked=false;
  starts++;

  
  for (;;){

    pthread_testcancel();
    CRef confl = propagate();
    
  
    if (confl != CRef_Undef){  
      conflicts++; conflictC++;
      if (verbosity >= 1 && conflicts%verbEveryConflicts==0){
	printf("c |%5d| %1d   %7d    %5d | %7d %8d %8d | %5d %8d   %6d %8d | %6.3f %% | \n",
	       id,  
	       (int)starts,(int)nbstopsrestarts, (int)(conflicts/starts), 
	       (int)dec_vars - (trail_lim.size() == 0 ? trail.size() : 
				trail_lim[0]), nClauses(), 
	       (int)clauses_literals, 
	       (int)nbReduceDB, nLearnts(), 
	       (int)nbDL2,(int)nbRemovedClauses, 
	       progressEstimate()*100);
      }
	
      if (decisionLevel() == 0) {
	std::cout << "Niveau 0"<<std::endl;
	return l_False;    
      }
	
      trailQueue.push(trail.size());
	
      if( conflicts>LOWER_BOUND_FOR_BLOCKING_RESTART && 
	  lbdQueue.isvalid()  && 
	  trail.size()>R*trailQueue.getavg()) {
	lbdQueue.fastclear();
	nbstopsrestarts++;
	if(!blocked) {
	  lastblockatrestart=starts;
	  nbstopsrestartssame++;
	  blocked=true;
	}
      }
	
      learnt_clause.clear();
      analyze(confl, learnt_clause, backtrack_level,nblevels);
	
      lbdQueue.push(nblevels);
      sumLBD += nblevels;
  
      if (nblevels <= maxsizeshared)
	sharedBase->push(learnt_clause, nblevels,this);
      
      cancelUntil(backtrack_level);
	    
      if (learnt_clause.size() == 1){
	uncheckedEnqueue(learnt_clause[0]);nbUn++;
      }else{
	CRef cr = ca.alloc(learnt_clause, true);
	ca[cr].setLBD(nblevels); 
	if(nblevels<=2) nbDL2++; // stats
	if(ca[cr].size()==2) nbBin++; // stats
	learnts.push(cr);

  /*RAJOUT STAGE*/

  if(ca[cr].setgel(to_freeze_var_act_clauses(cr, activity[order_heap[0]])) == 0){
    attachClause(cr);
    ca[cr].setused();
  }

  nb_clauses_total++;

//ca[cr].setgel(1);

/*
                Freeze_params fp = mkFreeze(0, to_freeze_var_act_clauses(cr,0));
                fp.freeze = 0;//to_freeze(cr);
                //frozen_clauses.push(fp);
*/
  /*FIN RAJOUT STAGE*/


	      
	claBumpActivity(ca[cr]);
	uncheckedEnqueue(learnt_clause[0], cr);
      }
      varDecayActivity();
      claDecayActivity();
	
    }else{

      // Our dynamic restart, see the SAT09 competition compagnion paper 
      if ((lbdQueue.isvalid() && 
	   ((lbdQueue.getavg()*K) > (sumLBD / conflicts)))) {
	
	lbdQueue.fastclear();
	progress_estimate = progressEstimate();
	cancelUntil(0);
	return l_Undef; 
      }

      // Simplify the set of problem clauses:
      if (decisionLevel() == 0 && !simplify()) {
	return l_False;
      }

      // Perform clause database reduction !
      if(conflicts>=(uint64_t)curRestart* nbclausesbeforereduce) 
	{	
	  assert(learnts.size()>0);
	  curRestart = (conflicts/ nbclausesbeforereduce)+1;
	  reduceDB();
	  nbclausesbeforereduce += incReduceDB;
	}
	    
      Lit next = lit_Undef;

      while (decisionLevel() < assumptions.size()){
	// Perform user provided assumption:
	Lit p = assumptions[decisionLevel()];
	if (value(p) == l_True){
	  // Dummy decision level:
	  newDecisionLevel();
	}else if (value(p) == l_False){
	  // analyzeFinal(~p, conflict);
	  return l_False;
	}else{
	  next = p;
	  break;
	}
      }
       
      if (next == lit_Undef){
	// New variable decision:
	decisions++;
	next = pickBranchLit();
	// The problem is solved 
	if (next == lit_Undef){
	  std::cout << "Search Thread="<< id <<std::endl;
	  return l_True;
	}
      }

      // Increase decision 
      // level and enqueue 'next'
      newDecisionLevel();
      uncheckedEnqueue(next);
    }
  }
}



//The CDCL procedure is encoded here
lbool pSolver::search( Lit next )
{
  assert(ok);
  int          backtrack_level;
  int          conflictC = 0;
  vec<Lit>     learnt_clause;
  unsigned int nblevels;
  bool         blocked=false;
  starts++;

  for (;;){
    
    pthread_testcancel();
    //Effectue la propagation unitaire, retourne une clause de conflit s'il y en a une
    CRef confl = propagate();
    //Si clause de conflit trouvée, alors
    if (confl != CRef_Undef){
      //Augmentation nb conflits (pour redémarrage?)
      conflicts++; conflictC++;
      //Affichage, hors analyse
      if (verbosity >= 1 && conflicts%verbEveryConflicts==0){
	printf("c |%5d| %1d   %7d    %5d | %7d %8d %8d | %5d %8d   %6d %8d | %6.3f %% |\n",
	       id,  
	       (int)starts,(int)nbstopsrestarts, (int)(conflicts/starts), 
	       (int)dec_vars - (trail_lim.size() == 0 ? trail.size() : 
				trail_lim[0]), nClauses(), 
	       (int)clauses_literals, 
	       (int)nbReduceDB, nLearnts(), 
	       (int)nbDL2,(int)nbRemovedClauses, 
	       progressEstimate()*100);
      }

      // Si le nombre d'éléments dans la liste trail_lim est égal à 0 => niveau de décision est égal à 0 aussi, alors retourner faux.
      if (decisionLevel() == 0) 
	return l_False;    
	
  //Sinon, inserer comme nouveau element, la taille de trail (pile d'affectations, donc sa taille est le nb d'affectations faites, niv de décision? Non à cause des
  // propagations unitaires). En tout cas enregistre la taille du nb de variables affectés => peut servir po
      trailQueue.push(trail.size());
	
    //Si le nombre de conflits a été dépassé, si la file lbd est pleine et si la taille du nb de variable affectés est plus grande que la taille de JE NE SAIS QUOI * R
    //Alors vars de lbdqueue à 0, et nb de stops-retarts augmente.
      if( conflicts>LOWER_BOUND_FOR_BLOCKING_RESTART && 
	  lbdQueue.isvalid()  && 
	  trail.size()>R*trailQueue.getavg()) {
	lbdQueue.fastclear();
	nbstopsrestarts++;
  //si pas bloqué, alors, last_block_at_restart = starts, bloqué = oui, nb stops restars pareil ++
	if(!blocked) {
	  lastblockatrestart=starts;
	  nbstopsrestartssame++;
	  blocked=true;
	}
      }
	   //Nettoyer le vecteur de clauses apprises.
      learnt_clause.clear();
      // Rempli nb levels
      analyze(confl, learnt_clause, backtrack_level,nblevels);
	    // Rajouter nblevels à la fin de lbdqueue
      lbdQueue.push(nblevels);
      // Somme des elems lbdqueue
      sumLBD += nblevels;

      //S'il reste de la place dans la file de partage, alors partager
      // la clause apprise
      if (nblevels <=maxsizeshared)
      	sharedBase->push(learnt_clause, nblevels,this);
      //Annule toutes les affectations faites, avec trail et trail_lim + decisions tableau
      //revient en quelque sorte au dernier niveau de decision
      cancelUntil(backtrack_level);
	    
      //Si après la propagation unitaire, la clause apprise est unitaire et non affectée encore, le faire, et créer une database pour elle.
      if (learnt_clause.size() == 1){
	uncheckedEnqueue(learnt_clause[0]);nbUn++;
      }else{
        //Si pas une clause unitaire, alors lui allouer de l'espace, lui donner son nombre de LBD, la mettre dans la liste des clauses apprises 
	CRef cr = ca.alloc(learnt_clause, true);
	ca[cr].setLBD(nblevels); 
	if(nblevels<=2) nbDL2++; // stats
	if(ca[cr].size()==2) nbBin++; // stats
	learnts.push(cr);
  //Attacher une clause à une "Watch List". AAAAH!!!! NOOOOOOON!!!

  /*RAJOUT STAGE*/


  if(ca[cr].setgel(to_freeze_var_act_clauses(cr, activity[order_heap[0]])) == 0){
    attachClause(cr);
    ca[cr].setused();
  }

  nb_clauses_total++;


//ca[cr].setgel(1);

/*
  Freeze_params fp = mkFreeze(0, to_freeze_var_act_clauses(cr,0));
  fp.freeze = 0;//to_freeze(cr);
  //frozen_clauses.push(fp);
*/
  /*FIN RAJOUT STAGE*/
	      
	claBumpActivity(ca[cr]);
	uncheckedEnqueue(learnt_clause[0], cr);
      }
      varDecayActivity();
      claDecayActivity();
	
    }else{

      // Our dynamic restart, see the SAT09 competition compagnion paper 
      if ((lbdQueue.isvalid() && 
	   ((lbdQueue.getavg()*K) > (sumLBD / conflicts)))) {
	lbdQueue.fastclear();
	progress_estimate = progressEstimate();
	cancelUntil(0);
	return l_Undef; 
      }

      // Simplify the set of problem clauses:
      if (decisionLevel() == 0 && !simplify()) {
	return l_False;
      }

      // Perform clause database reduction !
      if(conflicts>=(uint64_t)curRestart* nbclausesbeforereduce) 
	{	
	  assert(learnts.size()>0);
	  curRestart = (conflicts/ nbclausesbeforereduce)+1;
	  reduceDB();
	  nbclausesbeforereduce += incReduceDB;
	}	    
   
      if (next == lit_Undef){
	// New variable decision:
	decisions++;
	next = pickBranchLit();
	// The problem is solved 
	if (next == lit_Undef)
	  return l_True;
	
      }

      // Increase decision 
      // level and enqueue 'next'
      newDecisionLevel();
      uncheckedEnqueue(next);
      next=lit_Undef;
    }
  }
}

//In the portfolio phase, we stop all threads  
//when pfstopNB % of the workers have learned
//an amount of pfstopDB % of the size of original clauses' DB.
bool pSolver::grestart(pSolver *data[]){

  // static double const pfstopDB = 0.05; C'est un pourcentage!!! 5%.
  // (nb_clauses_apprises / taille_clauses_totale)
  // Tous les workers doivent apprendre au moins 5% de nouvelles clauses, par rapport à la taille initiale.
  int cpt =0;
  for (int i=1; i<nbworkers;++i){
    if((double)data[i]->learnts.size()/
       (double)data[i]->clauses.size() >= pfstopDB){
      cpt++;
    }
  }

  if(cpt < pfstopNB * (nbworkers-1))
     return false;

  return true;
}

//To start the divide an conquer phase, 
//we chose the liternals' order given 
//by the worker which progressed the best.
int pSolver::getbpick(pSolver *data[]){
  
  data[1]->cancelUntil(0);
  int max=1;

  for (int i=2; i<nbworkers;++i){
    data[i]->cancelUntil(0);
    
    if(data[max]->progressEstimate() <
       data[i]->progressEstimate()) 
      max = i;
    
  }
  
  for (int i=1; i<nbworkers;++i){
    if(i!=max){
      data[i]->order_heap.clear();
      data[max]->activity.copyTo(data[i]->activity);
      for(int j=0; j<data[max]->order_heap.size();++j)
	data[i]->insertVarOrder(data[max]->order_heap[j]);
    }
  }
  
  return max;
    
}


//The initialisation method. It starts 
//all other threads and wait for the
//termination of one of theim. 
void pSolver::solveMultiThreaded(int h, int t, pSolver *data[]){
  
  nbworkers=t;
  // getfirstLits();
  solvers=data;
  mpz_t ret;
  mpz_init(ret);
  mpz_set_si(ret,0);    
  bool rstart=true;
  int curr_restarts =0;
  double rest_base = restart_base; 
  long long cpt=0;
 

  if (!ok) return;

  sumLBD = 0;  
  lbdQueue.initSize(sizeLBDQueue);
  trailQueue.initSize(sizeTrailQueue);
  nbclausesbeforereduce = firstReduceDB;
 

   if(verbosity>=1) {
      printf("c ========================================[ MAGIC CONSTANTS ]==============================================\n");
      printf("c | Constants are supposed to work well together :-)                                                      |\n");
      printf("c | however, if you find better choices, please let us known...                                           |\n");
      printf("c |-------------------------------------------------------------------------------------------------------|\n");
      printf("c |                                |                                |                                     |\n"); 
      printf("c | - Restarts:                    | - Reduce Clause DB:            | - Minimize Asserting:               |\n");
      printf("c |   * LBD Queue    : %6d      |   * First     : %6d         |    * size < %3d                     |\n",lbdQueue.maxSize(),firstReduceDB,lbSizeMinimizingClause);
      printf("c |   * Trail  Queue : %6d      |   * Inc       : %6d         |    * lbd  < %3d                     |\n",trailQueue.maxSize(),incReduceDB,lbLBDMinimizingClause);
      printf("c |   * K            : %6.2f      |   * Special   : %6d         |                                     |\n",K,specialIncReduceDB);
      printf("c |   * R            : %6.2f      |   * Protected :  (lbd)< %2d     |                                     |\n",R,lbLBDFrozenClause);
      printf("c |                                |                                |                                     |\n"); 
      printf("c ==================================[ Search Statistics (every %6d conflicts) ]=========================\n",verbEveryConflicts);
      printf("c |                                                                                                       |\n");      
      printf("c |          RESTARTS           |          ORIGINAL         |              LEARNT              | Progress |\n");
      printf("c |Thrd | NB   Blocked  Avg Cfc |    Vars  Clauses Literals |   Red   Learnts    LBD2  Removed |          |\n");
      printf("c =========================================================================================================\n");
      printf("c =========================================================================================================\n");
      printf("c |                                          Mode Portfolio                                               |\n"); 
      printf("c =========================================================================================================\n");
   }


  nb_clauses_total = 0;
  nb_clauses_inutiles = 0;

   folio =true;
   // Le thread initil v creer un ensemble de "workers" ( = nb threads précisés => nb coeurs du systeme)
   // Crée un thread, avec bootstrap comme fonction, tetourne de TID, qu'on stoque précieusement dans la structure sharedJob.
   // Chaque thread disposera donc de data[i] => sa file de clauses à partager.
   for (int i=1; i<nbworkers; i++)
     sharedJob->tids[i-1]=create_worker(data[i]);
   
   while (true){
     
     usleep(5000);
     
     sharedJob->getResult(ret);
     // Attente 0.5 (??) secs, regarde le resultat obtenu  
     // Si ret != 0 , donc si VRAI, ALORS 
     if (stop(ret)){       
      // Annulation + attente de terminaison, car pthread_cancel provoque pthread_exit.
       for (int i=1; i<nbworkers; i++){
	 pthread_cancel(sharedJob->tids[i-1]);
	 pthread_join(sharedJob->tids[i-1], NULL);
       }

       if (verbosity >= 1)
	 printf("c =========================================================================================================\n");
       
       mpz_clear(ret);

       return;
     }

     // 5 % clauses nouvelles apprises
     if(grestart(data)){
       for (int i=1; i<nbworkers; i++){
	 pthread_cancel(sharedJob->tids[i-1]);
	 pthread_join(sharedJob->tids[i-1], NULL); 
       }

       folio =false;
       break;
     }
       
   }

   sharedJob->getResult(ret);
   if ( stop(ret)){
     if (verbosity >= 1)
       printf("c =========================================================================================================\n");
     
     mpz_clear(ret);
     return;
   }
   
   if (verbosity >= 1){
     printf("c =========================================================================================================\n");
     printf("c |                                          Mode Divide and Conquer                                      |\n"); 
     printf("c =========================================================================================================\n");
   }

   while(true){
     cpt++;

     if(rstart){

       sharedJob->init(nbworkers, h, 1<<h);

       int s = getbpick(data);

       if( data[s]->createJobs(h)){
	 mpz_set_si(ret, 1);
	 sharedJob->putResult(ret);
	 break;
       }
       else{
	 sharedJob->IFinish();

	 for (int i=1; i<t; i++)
	   sharedJob->tids[i-1] =  create_worker(data[i]);
       }
       rstart=false;
     }
    
     usleep(5000);
     sharedJob->getResult(ret);
       
     if (stop(ret)){
       
       for (int i=1; i<nbworkers; i++){
	 pthread_cancel(sharedJob->tids[i-1]);
	 pthread_join(sharedJob->tids[i-1], NULL);
       }
       break;
     }

    if(rest_base == cpt){
      if (verbosity >= 1){
	 printf("c =========================================================================================================\n");
	 printf("c |                                          Global Restart : %6d                                      |\n",curr_restarts+1); 
	 printf("c =========================================================================================================\n");
       }
       
       for (int i=1; i<nbworkers; i++){
	 pthread_cancel(sharedJob->tids[i-1]);
	 pthread_join(sharedJob->tids[i-1], NULL); 
       }
       
       rstart=true;
       rest_base =  pow(2, ++curr_restarts)*restart_base; 
    }
    
   }

  if (verbosity >= 1)
    printf("c =========================================================================================================\n");
  
  mpz_clear(ret);
}


void  pSolver::printStats() {
  printf("c Jobs done             : %d \n", nbjobs);
  printf("c CPU Thread time       : %g s\n", thtime);
  printf("c nb Shared learnts     : %d\n", sharedBase->getTotalDB(id));
  printf("c nb Red. Sh. learnts   : %d\n", sharedBase->getTotalSDB(id));
}
