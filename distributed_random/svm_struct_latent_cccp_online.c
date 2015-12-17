/* linear structural SVM with latent variables */
/* 30 September 2008 */

#include <stdio.h>
#include <assert.h>
//#include "./svm_light/svm_common.h"
//#include "svm_struct_latent_api_types.h"
#include "svm_struct_latent_api.h"

#include<time.h>
#include <stdlib.h>

#define ALPHA_THRESHOLD 1E-14
#define IDLE_ITER 20
#define CLEANUP_CHECK 100
#define STOP_PREC 1E-2
#define UPDATE_BOUND 3

#define MAX_OUTER_ITER 400

#define MAX(x,y) ((x) < (y) ? (y) : (x))
#define MIN(x,y) ((x) > (y) ? (y) : (x))

#define DEBUG_LEVEL 1

/* mosek interface */
int mosek_qp_optimize(double**, double*, double*, long, double, double*, double);

void my_read_input_parameters(int argc, char* argv[], char *trainfile, char *modelfile,
			      LEARN_PARM *learn_parm, KERNEL_PARM *kernel_parm, STRUCT_LEARN_PARM *struct_parm);

void my_wait_any_key();

int resize_cleanup(int size_active, int *idle, double *alpha, double *delta, double *gammaG0, double *proximal_rhs, double **G, DOC **dXc, double *cut_error);

double sprod_nn(double *a, double *b, long n) {
  double ans=0.0;
  long i;
  for (i=1;i<n+1;i++) {
    ans+=a[i]*b[i];
  }
  return(ans);
}

void add_vector_nn(double *w, double *dense_x, long n, double factor) {
  long i;
  for (i=1;i<n+1;i++) {
    w[i]+=factor*dense_x[i];
  }
}

double* add_list_nn(SVECTOR *a, long totwords) 
     /* computes the linear combination of the SVECTOR list weighted
	by the factor of each SVECTOR. assumes that the number of
	features is small compared to the number of elements in the
	list */
{
    SVECTOR *f;
    long i;
    double *sum;

    sum=create_nvector(totwords);

    for(i=0;i<=totwords;i++) 
      sum[i]=0;

    for(f=a;f;f=f->next){
      add_vector_ns(sum,f,f->factor);
    }


    return(sum);
}

SVECTOR* find_cutting_plane(EXAMPLE *ex, SVECTOR **fycache, double *margin, long m, STRUCTMODEL *sm,
		STRUCT_LEARN_PARM *sparm, char* tmpdir, char *trainfile, double frac_sim, double Fweight,
		char *dataset_stats_file, double rho_admm, long isExhaustive, long isLPrelaxation,
		double *margin2, int datasetStartIdx, int chunkSz, int eid, int chunkid) {

  long i;
  SVECTOR *f, *fy, *fybar, *lhs;
  LABEL       ybar;
  LATENT_VAR hbar;
  double lossval;
  double *new_constraint;

  long l,k;
  SVECTOR *fvec;
  WORD *words;  

  LABEL       *ybar_all = (LABEL*) malloc(sizeof(LABEL) * m);
  LATENT_VAR *hbar_all = (LATENT_VAR*) malloc (sizeof(LATENT_VAR) * m);
  time_t mv_start, mv_end;

  time(&mv_start);
  find_most_violated_constraint_marginrescaling_all_online(ybar_all, hbar_all, sm, sparm, m,
		  tmpdir, trainfile, frac_sim, dataset_stats_file, rho_admm, isExhaustive, isLPrelaxation,
		  Fweight, datasetStartIdx, chunkSz, eid, chunkid);
  time(&mv_end);

#if (DEBUG_LEVEL==1)
  print_time(mv_start, mv_end, "Max violators");
#endif


  /* find cutting plane */
  lhs = NULL;
  lossval = lossF1(ex, m, ybar_all, sparm, Fweight);
  *margin = lossval;

  *margin2 = 0;
  for (i=0;i<m;i++) {
    //find_most_violated_constraint_marginrescaling(ex[i].x, ex[i].y, &ybar, &hbar, sm, sparm);
    ybar = ybar_all[i];
    hbar = hbar_all[i];
    /* get difference vector */
    fy = copy_svector(fycache[i]);
    fybar = psi(ex[i].x,ybar,hbar,sm,sparm);
    lossval = loss(ex[i].y,ybar,hbar,sparm);
    free_label(ybar);
    free_latent_var(hbar);

    /* scale difference vector */
    for (f=fy;f;f=f->next) {
      f->factor*=1.0/m;
      //f->factor*=ex[i].x.example_cost/m;
    }

    for (f=fybar;f;f=f->next) {
      f->factor*=-1.0/m;
      //f->factor*=-ex[i].x.example_cost/m;
    }
    /* add ybar to constraint */
    append_svector_list(fy,lhs);
    append_svector_list(fybar,fy);
    lhs = fybar;
    *margin2+=lossval/m;
    //*margin+=lossval*ex[i].x.example_cost/m;
  }

  free(ybar_all);
  free(hbar_all);

  /* compact the linear representation */
  new_constraint = add_list_nn(lhs, sm->sizePsi);

//  printf("After this segfault ? \n");fflush(stdout);
//  printf("%x\n",new_constraint);

  free_svector(lhs);

  l=0;
  for (i=1;i<sm->sizePsi+1;i++) {
    if (fabs(new_constraint[i])>1E-10) l++; // non-zero
  }
  words = (WORD*)my_malloc(sizeof(WORD)*(l+1)); 
  assert(words!=NULL);
  k=0;
  for (i=1;i<sm->sizePsi+1;i++) {
    if (fabs(new_constraint[i])>1E-10) {
      words[k].wnum = i;
      words[k].weight = new_constraint[i]; 
      k++;
    }
  }
  words[k].wnum = 0;
  words[k].weight = 0.0;
  fvec = create_svector(words,"",1);

  free(words);
  free(new_constraint);

  return(fvec); 

}


double cutting_plane_algorithm(double *w, long m, int MAX_ITER, double C, double epsilon, SVECTOR **fycache, EXAMPLE *ex,
		STRUCTMODEL *sm, STRUCT_LEARN_PARM *sparm, char *tmpdir, char * trainfile, double frac_sim, double Fweight,
		char *dataset_stats_file, double rho_admm, long isExhaustive, long isLPrelaxation, double Cdash, int datasetStartIdx, int chunkSz,
		int eid, int chunkid, double *w_prev, int numChunks, int lossType) {
//	  printf("Addr. of w (inside cp_algo) %x\t%x\n",w,sm->w);
  long i,j;
  double xi;
  double *alpha;
  double **G; /* Gram matrix */
  DOC **dXc; /* constraint matrix */
  double *delta; /* rhs of constraints */
  SVECTOR *new_constraint;
  double dual_obj, alphasum;
  int iter, size_active; 
  double value;
  int r;
  int *idle; /* for cleaning up */
  double margin;
  double primal_obj;
  double *proximal_rhs;
  double *gammaG0=NULL;
  double min_rho = 0.001;
  double max_rho;
  double serious_counter=0;
  double rho = 1.0; /* temporarily set it to 1 first */

  double expected_descent, primal_obj_b=-1, reg_master_obj;
  int null_step=1;
  double *w_b;
  double kappa=0.1;
  double temp_var;
  double proximal_term, primal_lower_bound;

  double v_k; 
  double obj_difference; 
  double *cut_error; // cut_error[i] = alpha_{k,i} at current center x_k
  double sigma_k; 
  double m2 = 0.2;
  double m3 = 0.9;
  double gTd; 
  double last_sigma_k=0; 

  double initial_primal_obj;
  int suff_decrease_cond=0;
  double decrease_proportion = 0.2; // start from 0.2 first 

  double z_k_norm;
  double last_z_k_norm=0;

  w_b = create_nvector(sm->sizePsi);
  clear_nvector(w_b,sm->sizePsi);
  /* warm start */
  for (i=1;i<sm->sizePsi+1;i++) {
    w_b[i] = w[i];
  }

  iter = 0;
  size_active = 0;
  xi = 0.0;
  alpha = NULL;
  G = NULL;
  dXc = NULL;
  delta = NULL;
  idle = NULL;

  proximal_rhs = NULL;
  cut_error = NULL; 

  printf("ITER 0 \n(before cutting plane) \n");
  double margin2;
  new_constraint = find_cutting_plane (ex, fycache, &margin, m, sm, sparm, tmpdir, trainfile, frac_sim,
		  Fweight, dataset_stats_file, rho_admm, isExhaustive, isLPrelaxation, &margin2,
		  datasetStartIdx, chunkSz, eid, chunkid);
  value = margin2 - sprod_ns(w, new_constraint);

  margin -= sprod_ns(w_prev, new_constraint); //(Ajay: ONLINE LEARNING) IMPT NOTE --> constant addition to the loss ..
  	  	  	  	  	  	  	  	  	  	  	  // model score using w_prev values ('-' is used because the terms are reversed in the code)

  // Loss types: (Note: normalisation when applied, is applied to both hamming and F1 if it is a combination loss)
  switch(lossType){
  	  case 0:  // 0 --> combination (Hamming + F1) + unnormalised
  		  primal_obj_b = 0.5*sprod_nn(w_b,w_b,sm->sizePsi)+C*value + Cdash*margin; // Ajay: Change in obj involing both hamming and F1 loss;
  		  primal_obj = 0.5*sprod_nn(w,w,sm->sizePsi)+C*value + Cdash*margin; // Ajay: Change in obj involing both hamming and F1 loss;
  		  	  break;

  	  case 1: // 1 --> only F1 + normalised (by number of chunked datasets)
  		  primal_obj_b = 0.5*sprod_nn(w_b,w_b,sm->sizePsi)+ Cdash*margin/numChunks;
  		  primal_obj = 0.5*sprod_nn(w,w,sm->sizePsi)+ Cdash*margin/numChunks;
  		  	  break;

  	  case 2: // 2 --> only Hamming + normalised (by number of chunked datasets)
  		  primal_obj_b = 0.5*sprod_nn(w_b,w_b,sm->sizePsi)+C*value/numChunks ;
  		  primal_obj = 0.5*sprod_nn(w,w,sm->sizePsi)+C*value/numChunks ;
  		      break;

      case 3: // 3 --> combination (Hamming + F1) + normalised (by number of chunked datasets)
    	  primal_obj_b = 0.5*sprod_nn(w_b,w_b,sm->sizePsi)+C*value/numChunks + Cdash*margin/numChunks; // Ajay: Change in obj involing both hamming and F1 loss
    	  primal_obj = 0.5*sprod_nn(w,w,sm->sizePsi)+C*value/numChunks + Cdash*margin/numChunks; // Ajay: Change in obj involing both hamming and F1 loss;
  		  	  break;

  	  case 4:  // 4 --> only F1 + unnormalised
  		  primal_obj_b = 0.5*sprod_nn(w_b,w_b,sm->sizePsi)+ Cdash*margin;
  		  primal_obj = 0.5*sprod_nn(w,w,sm->sizePsi)+ Cdash*margin;
  		  	  break;

  	  case 5: // 5 --> only Hamming + unnormalised
  		  primal_obj_b = 0.5*sprod_nn(w_b,w_b,sm->sizePsi)+C*value;
  		  primal_obj = 0.5*sprod_nn(w,w,sm->sizePsi)+C*value;
  		   	  break;
  }

  primal_lower_bound = 0;
  expected_descent = -primal_obj_b;
  initial_primal_obj = primal_obj_b; 

  max_rho = C; 

  printf("Running CCCP inner loop solver: \n"); fflush(stdout);

  time_t iter_start, iter_end;

  while ((!suff_decrease_cond)&&(expected_descent<-epsilon)&&(iter<MAX_ITER)) { 
    iter+=1;
    size_active+=1;

    time(&iter_start);

#if (DEBUG_LEVEL>0)
    printf("ITER %d\n", iter); 
#endif
    printf("."); fflush(stdout); 

    /* add  constraint */
    dXc = (DOC**)realloc(dXc, sizeof(DOC*)*size_active);
    assert(dXc!=NULL);
    dXc[size_active-1] = (DOC*)malloc(sizeof(DOC));
    dXc[size_active-1]->fvec = new_constraint; 
    dXc[size_active-1]->slackid = 1; // only one common slackid (one-slack)
    dXc[size_active-1]->costfactor = 1.0;

    delta = (double*)realloc(delta, sizeof(double)*size_active);
    assert(delta!=NULL);
    delta[size_active-1] = margin2; // Ajay: changing for the formulation combining hamming and F1loss
    alpha = (double*)realloc(alpha, sizeof(double)*size_active);
    assert(alpha!=NULL);
    alpha[size_active-1] = 0.0;
    idle = (int*)realloc(idle, sizeof(int)*size_active);
    assert(idle!=NULL); 
    idle[size_active-1] = 0;
    /* proximal point */
    proximal_rhs = (double*)realloc(proximal_rhs, sizeof(double)*size_active);
    assert(proximal_rhs!=NULL); 
    cut_error = (double*)realloc(cut_error, sizeof(double)*size_active); 
    assert(cut_error!=NULL); 
    // note g_i = - new_constraint
    cut_error[size_active-1] = C*(sprod_ns(w_b, new_constraint) - sprod_ns(w, new_constraint)); 
    cut_error[size_active-1] += (primal_obj_b - 0.5*sprod_nn(w_b,w_b,sm->sizePsi)); 
    cut_error[size_active-1] -= (primal_obj - 0.5*sprod_nn(w,w,sm->sizePsi)); 

    gammaG0 = (double*)realloc(gammaG0, sizeof(double)*size_active);
    assert(gammaG0!=NULL);
      
    /* update Gram matrix */
    G = (double**)realloc(G, sizeof(double*)*size_active);
    assert(G!=NULL);
    G[size_active-1] = NULL;
    for (j=0;j<size_active;j++) {
      G[j] = (double*)realloc(G[j], sizeof(double)*size_active);
      assert(G[j]!=NULL);
    }
    for (j=0;j<size_active-1;j++) {
      G[size_active-1][j] = sprod_ss(dXc[size_active-1]->fvec, dXc[j]->fvec);
      G[j][size_active-1] = G[size_active-1][j];
    }
    G[size_active-1][size_active-1] = sprod_ss(dXc[size_active-1]->fvec,dXc[size_active-1]->fvec);

	
    /* update gammaG0 */
    if (null_step==1) {
      gammaG0[size_active-1] = sprod_ns(w_b, dXc[size_active-1]->fvec);
    } else {
      for (i=0;i<size_active;i++) {
	gammaG0[i] = sprod_ns(w_b, dXc[i]->fvec); 
      }
    }

     /* update proximal_rhs */
    for (i=0;i<size_active;i++) {
      proximal_rhs[i] = delta[i] - rho/(1+rho)*gammaG0[i];
    }


    /* solve QP to update alpha */
    dual_obj = 0; 
    time_t mosek_start, mosek_end;
    time(&mosek_start);
    r = mosek_qp_optimize(G, proximal_rhs, alpha, (long) size_active, C, &dual_obj,rho);
    time(&mosek_end);
#if(DEBUG_LEVEL == 1)
    print_time(mosek_start, mosek_end, "Mosek solver");
#endif
    /* DEBUG */
    //printf("r: %d\n", r); fflush(stdout);
    /* END DEBUG */

    clear_nvector(w,sm->sizePsi);
    for (j=0;j<size_active;j++) {
      if (alpha[j]>C*ALPHA_THRESHOLD) {
	add_vector_ns(w,dXc[j]->fvec,alpha[j]/(1+rho));
      }
    }

    z_k_norm = sqrt(sprod_nn(w,w,sm->sizePsi)); 

    add_vector_nn(w, w_b, sm->sizePsi, rho/(1+rho));

    
    /* detect if step size too small */
    sigma_k = 0; 
    alphasum = 0; 
    for (j=0;j<size_active;j++) {
      sigma_k += alpha[j]*cut_error[j]; 
      alphasum+=alpha[j]; 
    }
    sigma_k/=C; 
    gTd = -C*(sprod_ns(w,new_constraint) - sprod_ns(w_b,new_constraint));

#if (DEBUG_LEVEL>0)
    for (j=0;j<size_active;j++) {
      printf("alpha[%d]: %.8g, cut_error[%d]: %.8g\n", j, alpha[j], j, cut_error[j]);
    }
    printf("sigma_k: %.8g\n", sigma_k); 
    printf("alphasum: %.8g\n", alphasum);
    printf("g^T d: %.8g\n", gTd); 
    fflush(stdout); 
#endif


    /* update cleanup information */
    for (j=0;j<size_active;j++) {
      if (alpha[j]<ALPHA_THRESHOLD*C) {
	idle[j]++;
      } else {
        idle[j]=0;
      }
    }

  new_constraint = find_cutting_plane(ex, fycache, &margin, m, sm, sparm, tmpdir, trainfile,
		  frac_sim, Fweight, dataset_stats_file, rho_admm, isExhaustive, isLPrelaxation,
		  &margin2, datasetStartIdx, chunkSz, eid, chunkid);
 //   new_constraint = find_cutting_plane(ex, fycache, &margin, m, sm, sparm, tmpdir, trainfile, frac_sim, Fweight, dataset_stats_file, rho);
    value = margin2 - sprod_ns(w, new_constraint);

    margin -= sprod_ns(w_prev, new_constraint); //(Ajay: ONLINE LEARNING) IMPT NOTE --> constant addition to the loss ..
    	  	  	  	  	  	  	  	  	  	  	  // model score using w_prev values ('-' is used because the terms are reversed in the code)
    printf("sandip1729 margin : %.4f",margin);

    /* print primal objective */

    // Loss types: (Note: normalisation when applied, is applied to both hamming and F1 if it is a combination loss)
      switch(lossType){
      	  case 0:  // 0 --> combination (Hamming + F1) + unnormalised
      		  primal_obj = 0.5*sprod_nn(w,w,sm->sizePsi)+C*value + Cdash*margin; // Ajay: Change in obj involing both hamming and F1 loss;
      		  	  break;

      	  case 1: // 1 --> only F1 + normalised (by number of chunked datasets)
      		  primal_obj = 0.5*sprod_nn(w,w,sm->sizePsi)+ Cdash*margin/numChunks;
      		  	  break;

      	  case 2: // 2 --> only Hamming + normalised (by number of chunked datasets)
      		  primal_obj = 0.5*sprod_nn(w,w,sm->sizePsi)+C*value/numChunks ;
      		      break;

          case 3: // 3 --> combination (Hamming + F1) + normalised (by number of chunked datasets)
        	  primal_obj = 0.5*sprod_nn(w,w,sm->sizePsi)+C*value/numChunks + Cdash*margin/numChunks; // Ajay: Change in obj involing both hamming and F1 loss;
      		  	  break;

      	  case 4:  // 4 --> only F1 + unnormalised
      		  primal_obj = 0.5*sprod_nn(w,w,sm->sizePsi)+ Cdash*margin;
      		  	  break;

      	  case 5: // 5 --> only Hamming + unnormalised
      		  primal_obj = 0.5*sprod_nn(w,w,sm->sizePsi)+C*value;
      		   	  break;
      }
     
#if (DEBUG_LEVEL>0)
    printf("ITER PRIMAL_OBJ %.4f\n", primal_obj); fflush(stdout);
#endif
    
 
    temp_var = sprod_nn(w_b,w_b,sm->sizePsi); 
    proximal_term = 0.0;
    for (i=1;i<sm->sizePsi+1;i++) {
      proximal_term += (w[i]-w_b[i])*(w[i]-w_b[i]);
    }
    
    reg_master_obj = -dual_obj+0.5*rho*temp_var/(1+rho);
    expected_descent = reg_master_obj - primal_obj_b;

    v_k = (reg_master_obj - proximal_term*rho/2) - primal_obj_b; 

    primal_lower_bound = MAX(primal_lower_bound, reg_master_obj - 0.5*rho*(1+rho)*proximal_term);

#if (DEBUG_LEVEL>0)
    printf("ITER REG_MASTER_OBJ: %.4f\n", reg_master_obj);
    printf("ITER EXPECTED_DESCENT: %.4f\n", expected_descent);
    printf("ITER PRIMLA_OBJ_B: %.4f\n", primal_obj_b);
    printf("ITER RHO: %.4f\n", rho);
    printf("ITER ||w-w_b||^2: %.4f\n", proximal_term);
    printf("ITER PRIMAL_LOWER_BOUND: %.4f\n", primal_lower_bound);
    printf("ITER V_K: %.4f\n", v_k); 
#endif
    obj_difference = primal_obj - primal_obj_b; 


    if (primal_obj<primal_obj_b+kappa*expected_descent) {
      /* extra condition to be met */
      if ((gTd>m2*v_k)||(rho<min_rho+1E-8)) {
#if (DEBUG_LEVEL>0)
	printf("SERIOUS STEP\n");
#endif
	/* update cut_error */
	for (i=0;i<size_active;i++) {
	  cut_error[i] -= (primal_obj_b - 0.5*sprod_nn(w_b,w_b,sm->sizePsi)); 
	  cut_error[i] -= C*sprod_ns(w_b, dXc[i]->fvec); 
	  cut_error[i] += (primal_obj - 0.5*sprod_nn(w,w,sm->sizePsi));
	  cut_error[i] += C*sprod_ns(w, dXc[i]->fvec); 
	}
	primal_obj_b = primal_obj;
	for (i=1;i<sm->sizePsi+1;i++) {
	  w_b[i] = w[i];
	}
	null_step = 0;
	serious_counter++;	
      } else {
	/* increase step size */
#if (DEBUG_LEVEL>0)
	printf("NULL STEP: SS(ii) FAILS.\n");
#endif
	serious_counter--; 
	rho = MAX(rho/10,min_rho);
      }
    } else { /* no sufficient decrease */
      serious_counter--; 
      if ((cut_error[size_active-1]>m3*last_sigma_k)&&(fabs(obj_difference)>last_z_k_norm+last_sigma_k)) {
#if (DEBUG_LEVEL>0)
	printf("NULL STEP: NS(ii) FAILS.\n");
#endif
	rho = MIN(10*rho,max_rho);
      } 
#if (DEBUG_LEVEL>0)
      else printf("NULL STEP\n");
#endif
    }
    /* update last_sigma_k */
    last_sigma_k = sigma_k; 
    last_z_k_norm = z_k_norm; 


    /* break away from while loop if more than certain proportioal decrease in primal objective */
    if (primal_obj_b/initial_primal_obj<1-decrease_proportion) {
      suff_decrease_cond = 1; 
    }

    /* clean up */
    if (iter % CLEANUP_CHECK == 0) {
      size_active = resize_cleanup(size_active, idle, alpha, delta, gammaG0, proximal_rhs, G, dXc, cut_error);
    }

	time(&iter_end);

#if (DEBUG_LEVEL==1)
	char msg[20];
	sprintf(msg,"ITER %d",iter);
    print_time(iter_start, iter_end, msg);
#endif
  } // end cutting plane while loop 

  printf(" Inner loop optimization finished.\n"); fflush(stdout); 
  //printf("@#$^() iteration no. - %d ",iteration);
      
  /* free memory */
  for (j=0;j<size_active;j++) {
    free(G[j]);
    free_example(dXc[j],0);	
  }
  free(G);
  free(dXc);
  free(alpha);
  free(delta);
  free_svector(new_constraint);
  free(idle);
  free(gammaG0);
  free(proximal_rhs);
  free(cut_error); 

  /* copy and free */
  for (i=1;i<sm->sizePsi+1;i++) {
    w[i] = w_b[i];
  }
  free(w_b);

  return(primal_obj_b);

}

// chunkid should start from 1
void create_chunk(EXAMPLE *dataset, long datasetStartIdx, SAMPLE *sample, long chunkSz){

		//SAMPLE *sample = (SAMPLE*)malloc(sizeof(SAMPLE));

		long eg_id;
		int num_mentions, num_rels, total_num_rels;

		// init. 'SAMPLE'
		sample->n = chunkSz;
		sample->examples = (EXAMPLE*)malloc(sizeof(EXAMPLE)*chunkSz);

		//printf("sample->examples %x\n", sample->examples);

		for(eg_id = 0; eg_id < chunkSz; eg_id ++){
			//printf("----\nEg : %ld\n", eg_id);

			//init 'EXAMPLE'
			EXAMPLE *e = &(sample->examples[eg_id]);

			//printf("%x\t%x\n",e,sample->examples);

			num_rels = dataset[datasetStartIdx + eg_id].y.num_relations;
			//printf("Num_relations %d\n", num_rels);

			//init 'LABEL' (e->y)
			e->y.num_relations = num_rels;

			if(num_rels > 0) {
				int yid;
				e->y.relations = (int*)malloc((sizeof(int))*num_rels);

//				printf("e->y.relations = %x\n", e->y.relations);
				for(yid = 0; yid < num_rels; yid++){
					//printf("Ajay -1\n");
					e->y.relations[yid] = dataset[datasetStartIdx + eg_id].y.relations[yid]; // --> eg. i -- relation label
					//printf("Ajay -2 %d\n", dataset[datasetStartIdx + eg_id].y.relations[yid]);

//					printf("e->y.relations = %x\n", e->y.relations);
//					printf("e->y.relations = %d\n", *(e->y.relations));
//					printf("Ajay %d\n ",e->y.relations[yid]);
//					printf("e->y.relations = %d\n", *(e->y.relations));
				}
				//printf("\n");
			}

			num_mentions = dataset[datasetStartIdx + eg_id].x.num_mentions; // --> eg. i -- no. of mention labels
//			printf("Num_mentions %d\n", num_mentions);

			// init 'PATTERN' (e->x)
			e->x.num_mentions = num_mentions;
			e->x.mention_features = (SVECTOR*)malloc(sizeof(SVECTOR)*num_mentions);
//			printf("Ajay -3 \t e->x.mention_features = %x\n", e->x.mention_features);
			// init 'LATENT_VAR' (e->h)
			e->h.num_mentions = num_mentions;
			// Each of the mention labels should be initialized to nil label
			// But we do not have a specific nil label index now.
			// Right now initialising to 0 (nillabel)
			e->h.mention_labels = (int*) malloc(sizeof(int)*num_mentions);

//			printf("Ajay -4\n");
			int i;
			for(i = 0; i < num_mentions; i ++){
				e->h.mention_labels[i] = 0;
			}
//			printf("Ajay -5\nNum mentions : %d\n", num_mentions);
			int m;
			for(m = 0; m < num_mentions; m++){
//				printf("Ajay -6- save\n");
//				printf("Ajay -6.5 sz %s\n", (dataset[datasetStartIdx+eg_id].x.mention_features[m].userdefined));
				//printf("(sz:%d)\t",f_sz);
				char * f_sz_str = NULL;
//				printf(" Ajay -6 (null str) %x\n", f_sz_str);
				f_sz_str = (char*)malloc(sizeof(char)*10);// --> eg. i, men m -- sz of the Fvector
//				printf("Ajay -6 (allocated str) %x\n", f_sz_str);
				strncpy(f_sz_str, dataset[datasetStartIdx+eg_id].x.mention_features[m].userdefined, 10);
//				printf("Ajay -6.6 %x\n", f_sz_str);
				int f_sz = atoi(f_sz_str);
//				printf("Ajay -6.62 %d\n", f_sz);
				e->x.mention_features[m].userdefined =  f_sz_str;
//				printf("Ajay -6.63 %x\n", e->x.mention_features[m].userdefined);
//				printf("Ajay -7\te->x.mention_features[m].userdefined = %s \t e->x.mention_features[0].words = %x\n", e->x.mention_features[m].userdefined, e->x.mention_features[0].userdefined);
				e->x.mention_features[m].words = (WORD*)malloc(sizeof(WORD)*(f_sz + 1));

//				printf("Ajay -8 \t e->x.mention_features[0].words \n", e->x.mention_features[0].words);

				int i;
				for(i = 0; i < f_sz; i ++){
					int f_id; float f_val;
					//fscanf(fp, "%d:%f ", &f_id, &f_val);
					// --> eg. i, men m -- Fvector (<fid:freq> <fid:freq> ....)
					//printf("%d:%.1f ", f_id, f_val);

					e->x.mention_features[m].words[i].wnum = dataset[datasetStartIdx+eg_id].x.mention_features[m].words[i].wnum;
					e->x.mention_features[m].words[i].weight = dataset[datasetStartIdx+eg_id].x.mention_features[m].words[i].weight;

				}

//				printf("Ajay -8\n");
				// Add 0 to the last word ... might be necessary somewhere
				e->x.mention_features[m].words[i].wnum = 0;
				e->x.mention_features[m].words[i].weight = 0;

//				printf("Ajay -9\n");
				//printf("\n");
			}
		}

		//printf("AddAj : %x\t%x\t%x\n",sample->examples->y.relations, sample->examples->x.mention_features, sample->examples->h.mention_labels);
		//return (sample);
}

SAMPLE * split_data(SAMPLE *sample, int numChunks, int randomize){

//	printf("Entire Dataset : \n");
//	test_print(*sample);
//	printf("---------------------------------------------------------------------\n");
//	printf("---------------------------------------------------------------------\n");

	int dataset_sz = sample->n;
	EXAMPLE *dataset = sample->examples;

	// randomize the datapoints
//	if(randomize == 1){
//		randomize_dataset(sample);
//	}

	int i;
	long chunkSz = dataset_sz / numChunks;
	SAMPLE *chunks = (SAMPLE*) malloc(sizeof(SAMPLE)*numChunks) ;

//	printf("Individual Chunks");
//	printf("---------------------------------------------------------------------\n");

	long datasetStartIdx;
	// create until the last but one chunk
	SAMPLE* c;
	for(i = 1; i <= numChunks - 1; i++){
		datasetStartIdx = (i-1)*chunkSz;
		printf("(OnlineSVM): Creating %d chunk \n",i );
		create_chunk(dataset, datasetStartIdx, &(chunks[i-1]), chunkSz);
		//chunks[i-1] = *c;

		//printf("AddAj-1 : %x\t%x\t%x\n",chunk->examples->y.relations, chunk->examples->x.mention_features, chunk->examples->h.mention_labels);

//		printf("Chunk-id - %d", i);
//		printf("---------------------------------------------------------------------\n");
//		test_print(*chunk);
//		printf("---------------------------------------------------------------------\n");
	}
	// create the last chunk
	datasetStartIdx = (numChunks-1) * chunkSz; // Calculate the startoffset using the old chunk sz
	chunkSz = dataset_sz - (numChunks-1) * chunkSz; // Calculate the chunk size of the last chunk
	create_chunk(dataset, datasetStartIdx, &(chunks[numChunks-1]), chunkSz);
	printf("(OnlineSVM): Creating %d chunk \n", numChunks);
	//chunks[numChunks-1] = *c;

	//printf("AddAj-1 : %x\t%x\t%x\n",chunk->examples->y.relations, chunk->examples->x.mention_features, chunk->examples->h.mention_labels);

//	printf("Chunk-id - %d", numChunks);
//	printf("---------------------------------------------------------------------\n");
//	test_print(*chunk);
//	printf("---------------------------------------------------------------------\n");

//	printf("DONE PRINTING .. COME HERE PRONTO\n");

	//exit(0);
	return chunks;
}

void create_u_variables(double ***w_iters, int eid, int chunkid, int numChunks, STRUCTMODEL *sm, double *zeroes, double *w_prev_epoch){

	int i;
	  // -------------------------------------------------------------------------------------------
	  /**
	   * (Online Learning): Create new variable u = w - w^(i-1)
	   */
		/*if(chunkid == 0 && eid == 0){	// First Chunk of First Epoch
			  for(i = 0; i < sm->sizePsi+1; i++){
				  w_iters[eid][chunkid][i] = w_iters[eid][chunkid][i] - zeroes[i];
			  }
		}
		else if(chunkid == 0){ 			// First chunk of the new Epoch
			  for(i = 0; i < sm->sizePsi+1; i++){
				  w_iters[eid][chunkid][i] = w_iters[eid][chunkid][i] - w_iters[eid-1][numChunks-1][i];
			  }
		}
		else {
			  for(i = 0; i < sm->sizePsi+1; i++){
				  w_iters[eid][chunkid][i] = w_iters[eid][chunkid][i] - w_iters[eid][chunkid-1][i];
			  }
		}*/

		for(i = 0; i < sm->sizePsi+1; i++){
			  w_iters[eid][chunkid][i] = w_iters[eid][chunkid][i] - w_prev_epoch[i];
		}
	  // -------------------------------------------------------------------------------------------

}

void restore_w_variables(double ***w_iters, int eid, int chunkid, int numChunks, STRUCTMODEL *sm, double *zeroes, double *w_prev_epoch){
	int i;
	  // -------------------------------------------------------------------------------------------
	  /**
	   * (Online Learning): 'Re-add' the w^(i) variables to 'u',
	   * output from the cutting plane algorithm code from the previous iteration,
	   * to actually compute 'w'
	   */
		/*if(chunkid == 0 && eid == 0){	// First Chunk of First Epoch
			  for(i = 0; i < sm->sizePsi+1; i++){
				  w_iters[eid][chunkid][i] = w_iters[eid][chunkid][i] + zeroes[i];
			  }
		}
		else if(chunkid == 0){ 			// First chunk of the new Epoch
			  for(i = 0; i < sm->sizePsi+1; i++){
				  w_iters[eid][chunkid][i] = w_iters[eid][chunkid][i] + w_iters[eid-1][numChunks-1][i];
			  }
		}
		else {
			  for(i = 0; i < sm->sizePsi+1; i++){
				  w_iters[eid][chunkid][i] = w_iters[eid][chunkid][i] + w_iters[eid][chunkid-1][i];
			  }
		}*/
		for(i = 0; i < sm->sizePsi+1; i++){
			  w_iters[eid][chunkid][i] = w_iters[eid][chunkid][i] + w_prev_epoch[i];
		}
	  // -------------------------------------------------------------------------------------------
}

double optimizeMultiVariatePerfMeasure(SAMPLE sample, int datasetStartIdx, int chunkSz, STRUCTMODEL *sm, STRUCT_LEARN_PARM *sparm,
		double C, double Cdash, double epsilon, int MAX_ITER, LEARN_PARM *learn_parm, char *trainfile,
		double ***w_iters, int eid, int chunkid, int numChunks, double *zeroes, double *w_prev_epoch){

	int i;
	time_t time_start, time_end;
	double decrement;
	double primal_obj, last_primal_obj;
	double cooling_eps;
	double stop_crit;
	LATENT_VAR *imputed_h = NULL;

	int dataset_sz = sample.n;
	SVECTOR **fycache, *diff, *fy;
	EXAMPLE *ex = sample.examples;

	/* some training information */
	printf("C: %.8g\n", C);
	printf("Cdash: %.8g\n", Cdash);
	printf("epsilon: %.8g\n", epsilon);
	printf("sample.n: %ld\n", dataset_sz);
	printf("sm->sizePsi: %ld\n", sm->sizePsi); fflush(stdout);

	printf("Loss Type : %d\n",learn_parm->lossType);
	printf("--\n");
	switch(learn_parm->lossType){
		case 0: printf("0 --> combination (Hamming + F1) + unnormalised\n");
				break;
		case 1: printf("1 --> only F1 + normalised (by number of chunked datasets)\n");
				break;
		case 2: printf("2 --> only Hamming + normalised (by number of chunked datasets)\n");
				break;
		case 3: printf("3 --> combination (Hamming + F1) + normalised (by number of chunked datasets)\n");
				break;
		case 4: printf("4 --> only F1 + unnormalised\n");
				break;
		case 5: printf("5 --> only Hamming + unnormalised\n");
				break;
	}
	printf("-----\n");

	/* prepare feature vector cache for correct labels with imputed latent variables */
	fycache = (SVECTOR**)malloc(dataset_sz*sizeof(SVECTOR*));
	for (i=0;i<dataset_sz;i++) {
		fy = psi(ex[i].x, ex[i].y, ex[i].h, sm, sparm);
		diff = add_list_ss(fy);
		free_svector(fy);
		fy = diff;
		fycache[i] = fy;
	}

	/* time taken stats */
	time(&time_start);

	/* outer loop: latent variable imputation */
	int outer_iter = 0;
	last_primal_obj = 0;
	decrement = 0;
	cooling_eps = 0.5*MAX(C,Cdash)*epsilon;
	while ((outer_iter<2)||((!stop_crit)&&(outer_iter<MAX_OUTER_ITER))) {
		printf("OUTER ITER %d\n", outer_iter); fflush(stdout);
		/* cutting plane algorithm */
		time_t cp_start, cp_end;
		time(&cp_start);

		/// NOTE : Change of variables (Create 'u' by subtracting w_prev from w)
		create_u_variables(w_iters, eid, chunkid, numChunks, sm, zeroes, w_prev_epoch);

		/*if(chunkid == 0 && eid == 0){ // First Chunk of First Epoch
			primal_obj = cutting_plane_algorithm(w_iters[eid][chunkid], dataset_sz, MAX_ITER, C, cooling_eps,
							fycache, ex, sm, sparm,	learn_parm->tmpdir, trainfile, learn_parm->frac_sim,
							learn_parm->Fweight, learn_parm->dataset_stats_file, learn_parm->rho_admm,
							learn_parm->isExhaustive, learn_parm->isLPrelaxation, Cdash, datasetStartIdx, chunkSz,
							eid, chunkid, zeroes, numChunks, learn_parm->lossType); // pass the zeroes vector
		}
		else if(chunkid == 0){ // First chunk of the new Epoch
			primal_obj = cutting_plane_algorithm(w_iters[eid][chunkid], dataset_sz, MAX_ITER, C, cooling_eps,
							fycache, ex, sm, sparm,	learn_parm->tmpdir, trainfile, learn_parm->frac_sim,
							learn_parm->Fweight, learn_parm->dataset_stats_file, learn_parm->rho_admm,
							learn_parm->isExhaustive, learn_parm->isLPrelaxation, Cdash, datasetStartIdx, chunkSz,
							eid, chunkid, w_iters[eid-1][numChunks-1], numChunks, learn_parm->lossType); // Last chunk of previous epoch
		}
		else {
			primal_obj = cutting_plane_algorithm(w_iters[eid][chunkid], dataset_sz, MAX_ITER, C, cooling_eps,
							fycache, ex, sm, sparm,	learn_parm->tmpdir, trainfile, learn_parm->frac_sim,
							learn_parm->Fweight, learn_parm->dataset_stats_file, learn_parm->rho_admm,
							learn_parm->isExhaustive, learn_parm->isLPrelaxation, Cdash, datasetStartIdx, chunkSz,
							eid, chunkid, w_iters[eid][chunkid-1], numChunks, learn_parm->lossType); // previous chunk id of current epoch
		}*/

		primal_obj = cutting_plane_algorithm(w_iters[eid][chunkid], dataset_sz, MAX_ITER, C, cooling_eps,
									fycache, ex, sm, sparm,	learn_parm->tmpdir, trainfile, learn_parm->frac_sim,
									learn_parm->Fweight, learn_parm->dataset_stats_file, learn_parm->rho_admm,
									learn_parm->isExhaustive, learn_parm->isLPrelaxation, Cdash, datasetStartIdx, chunkSz,
									eid, chunkid, w_prev_epoch, numChunks, learn_parm->lossType);

		time(&cp_end);

#if(DEBUG_LEVEL==1)
		char msg[20];
		sprintf(msg,"OUTER ITER %d", outer_iter);
		print_time(cp_start, cp_end, msg);
#endif

		/* compute decrement in objective in this outer iteration */
		decrement = last_primal_obj - primal_obj;
		last_primal_obj = primal_obj;
		printf("primal objective: %.4f\n", primal_obj);
		printf("decrement: %.4f\n", decrement); fflush(stdout);

		stop_crit = (decrement<MAX(C, Cdash)*epsilon)&&(cooling_eps<0.5*MAX(C, Cdash)*epsilon+1E-8);

		cooling_eps = -decrement*0.01;
		cooling_eps = MAX(cooling_eps, 0.5*MAX(C,Cdash)*epsilon);
		printf("cooling_eps: %.8g\n", cooling_eps);


		/* impute latent variable using updated weight vector */
		for(i = 0; i < dataset_sz; i ++)
			free_latent_var(ex[i].h);
		if(imputed_h != NULL)
			free(imputed_h);

		imputed_h = (LATENT_VAR*)malloc(sizeof(LATENT_VAR) * dataset_sz);
		infer_latent_variables_all(imputed_h, sm, sparm, dataset_sz, learn_parm->tmpdir, trainfile, datasetStartIdx, chunkSz, eid, chunkid);

		for (i=0;i<dataset_sz;i++) {
			//      free_latent_var(ex[i].h);
			//      ex[i].h = infer_latent_variables(ex[i].x, ex[i].y, &sm, &sparm); // ILP for  Pr (Z | Y_i, X_i) in our case
			ex[i].h = imputed_h[i];
		}
		/* re-compute feature vector cache */
		for (i=0;i<dataset_sz;i++) {
			free_svector(fycache[i]);
			fy = psi(ex[i].x, ex[i].y, ex[i].h, &sm, &sparm);
			diff = add_list_ss(fy);
			free_svector(fy);
			fy = diff;
			fycache[i] = fy;
		}
		printf("(OnlineSVM) .. finished outer_iter %d\n",outer_iter);
		outer_iter++;

		/// NOTE: Restore the 'w' by adding the current 'u' to w_prev
		restore_w_variables(w_iters, eid, chunkid, numChunks, sm, zeroes, w_prev_epoch);

	} // end outer loop

	time(&time_end);

	#if (DEBUG_LEVEL==1)
	  print_time(time_start, time_end, "Total time");
	#endif

	for(i=0;i<dataset_sz;i++) {
		free_svector(fycache[i]);
	}
	free(fycache);

	return primal_obj;
}

void copy_vector(double *dst, double *src, int sz){

	int i;
	for(i = 0; i <= sz; i++){ // +1 is added to sz as in feature vectors previously defined.
		dst[i] = src[i];
	}
}

int main(int argc, char* argv[]) {

  // The file to create the online version of the code

//  printf("Runs with F1 loss in the loss-augmented objective .. only positive data .. with weighting of Fscores .. no regions file");
  printf("Runs with loss types specified via a parameter\n");

//  double *w; /* weight vector */
  double C, epsilon, Cdash;
  LEARN_PARM learn_parm;
  KERNEL_PARM kernel_parm;
  char trainfile[1024];
  char modelfile[1024];
  int MAX_ITER;
  int counter;

  SAMPLE sample;
  STRUCT_LEARN_PARM sparm;
  STRUCTMODEL sm;

  /* read input parameters */
  my_read_input_parameters(argc, argv, trainfile, modelfile, &learn_parm, &kernel_parm, &sparm);

  epsilon = learn_parm.eps;
  C = learn_parm.svm_c;
  Cdash = learn_parm.Cdash;
  MAX_ITER = learn_parm.maxiter;

  /* read in examples */
  //strcpy(trainfile, "dataset/reidel_trainSVM.small.data");
  sample = read_struct_examples(trainfile,&sparm);
  
  /* initialization */
  init_struct_model(sample,&sm,&sparm,&learn_parm,&kernel_parm);

  // (OnlineSVM : Commenting 'w' as they are replaced by 'w_iters'
//  w = create_nvector(sm.sizePsi);
//  clear_nvector(w, sm.sizePsi);
//  sm.w = w; /* establish link to w, as long as w does not change pointer */

		double *zeroes = create_nvector(sm.sizePsi);
		clear_nvector(zeroes, sm.sizePsi);

//  printf("Addr. of w (init) %x\t%x\n",w,sm.w);

 		time_t time_start_full, time_end_full;
		int eid,totalEpochs=learn_parm.totalEpochs;
		int chunkid, numChunks=learn_parm.numChunks;

		double primal_obj_sum, primal_obj;
		char chunk_trainfile[1024];
		SAMPLE * chunk_dataset = (SAMPLE *) malloc(sizeof(SAMPLE)*numChunks);

		/**
			   * If we have ‘k’ instances and do ‘n’ epochs, after processing each chunk we update the weight.
			   * Since we do ‘k’ updates, we will have ‘k’ weight vectors after each epoch.
			   * After ‘n’ epochs, we will have ‘k*n’ weight vectors.
		*/
		// --------------------------------------------------------------------------------------------------------------------------------
		double ***w_iters = (double**) malloc(totalEpochs*sizeof(double**));
//		printf("--2: After 1st malloc -- %x; sz = %d\n", w_iters, totalEpochs*sizeof(double**));
		for(eid = 0; eid < totalEpochs; eid++){
			w_iters[eid] = (double*) malloc(numChunks*sizeof(double*));
//			printf("2.5... id = %d, .. allocated ... %x; sz = %d\n",eid, w_iters[eid],numChunks*sizeof(double*));
		}
		printf("--3: After 2nd malloc \n");
		for(eid = 0; eid < totalEpochs; eid++){
			for(chunkid = 0; chunkid < numChunks; chunkid++){
				w_iters[eid][chunkid] = create_nvector(sm.sizePsi);
//				printf("Confirming memory location : %x\n",w_iters[eid][chunkid]);
				clear_nvector(w_iters[eid][chunkid], sm.sizePsi);
			}
		}
		sm.w_iters = w_iters;
		printf("(ONLINE SVM) Completed the memory alloc for the parameters\n");
		// --------------------------------------------------------------------------------------------------------------------------------
		printf("XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX Changed .... Calling Java to split dataset for EPOCH %d\n", eid);
		char *cmd = malloc(1000);
		strcpy(cmd,"java -Xmx1G -cp java/bin:java/lib/* "
				" javaHelpers.splitDataset ");
		strcat(cmd, trainfile);
		strcat(cmd, " ");
		char numChunks_str[10]; sprintf(numChunks_str, "%d", numChunks);
		strcat(cmd, numChunks_str);
		strcat(cmd, " ");

		printf("Executing cmd : %s\n", cmd);fflush(stdout);
		system(cmd);
		// --------------------------------------------------------------------------------------------------------------------------------

		for(chunkid = 0; chunkid < numChunks; chunkid++)
		{
			memset(chunk_trainfile, 0, 1024);
			strcat(chunk_trainfile,trainfile);
			strcat(chunk_trainfile,".chunks/chunk."); // NOTE: Name hard-coded according to the convention used to create chunked files
			char chunkid_str[10];sprintf(chunkid_str, "%d", chunkid);
			strcat(chunk_trainfile,chunkid_str);
			printf("XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX Changed .... Reading chunked dataset\n");
			printf("Chunk trainfile : %s\n",chunk_trainfile);
			chunk_dataset[chunkid] = read_struct_examples_chunk(chunk_trainfile);
		}

		/**
		 * Sandip - Distr - Creating the w_prev for each EPOCH
		 */
		double **w_prev_epoch = (double*) malloc((totalEpochs+1)*sizeof(double*));
		for(eid = 0; eid < totalEpochs+1; eid++){
			w_prev_epoch[eid] = create_nvector(sm.sizePsi);
			clear_nvector(w_prev_epoch[eid], sm.sizePsi);
		}
		/*** End ***/

		/**
		 * Having divided the dataset (X,Y) into set of 'k' chunks / sub-datasets (X_1,Y_1) ... (X_k, Y_k)
		 * Do the following do while routine for one set of datapoints (sub-datasets)
		 */
		// --------------------------------------------------------------------------------------------------------------------------------

		time(&time_start_full);
		for(eid = 0; eid < totalEpochs; eid++)
		{


		int no_of_mchns = learn_parm.machines;
//		int avg_counter = 0;

		// Select a random group of chunk
		srand(time(NULL));
		int group = rand()%(numChunks/no_of_mchns);

		/**
		 * Sandip - Distr - Added to get avg
		 */
		double **w_avg = (double*) malloc(no_of_mchns*sizeof(double*));
		for(counter = 0; counter < no_of_mchns; counter++){
			w_avg[counter] = create_nvector(sm.sizePsi);
			clear_nvector(w_avg[counter], sm.sizePsi);
		}
		/*** End ***/

			printf("(ONLINE LEARNING) : EPOCH %d\n",eid);
			primal_obj_sum = 0.0;
			for(chunkid = no_of_mchns*group; chunkid < no_of_mchns*group + no_of_mchns; chunkid++) // NOTE: Chunkid starts from 1 and goes upto numChumks
			{

				int sz = sample.n / numChunks;
				int datasetStartIdx = (chunkid) * sz;
//				int chunkSz = (numChunks-1 == chunkid) ? (sample.n - ((numChunks-1)*sz) ) : (sz);

				int chunkSz = chunk_dataset[chunkid].n;

				//Sandip-1729 Added Chunk File Name
				memset(chunk_trainfile, 0, 1024);
				strcat(chunk_trainfile,trainfile);
				strcat(chunk_trainfile,".chunks/chunk."); // NOTE: Name hard-coded according to the convention used to create chunked files
				char chunkid_str[10];sprintf(chunkid_str, "%d", chunkid);
				strcat(chunk_trainfile,chunkid_str);

				primal_obj = optimizeMultiVariatePerfMeasure(chunk_dataset[chunkid], datasetStartIdx, chunkSz,
										&sm, &sparm, C, Cdash, epsilon, MAX_ITER, &learn_parm, chunk_trainfile,
										w_iters, eid, chunkid, numChunks, zeroes, w_prev_epoch[eid]);

//				primal_obj = optimizeMultiVariatePerfMeasure(chunk_dataset[chunkid], datasetStartIdx, chunkSz,
//						&sm, &sparm, C, Cdash, epsilon, MAX_ITER, &learn_parm, trainfile, w_iters, eid, chunkid, numChunks, zeroes);

				printf("(ONLINE LEARNING) : FINISHED PROCESSING CHUNK (PSEUDO-DATAPOINT) %d of %d\n",chunkid+1, numChunks);
				primal_obj_sum += primal_obj;
				printf("(OnlineSVM) : Processed pseudo-datapoint -- primal objective sum: %.4f\n", primal_obj_sum);

//				w_avg[avg_counter] = w_iters[eid][chunkid];

				//Get the avg
				add_vector_nn(w_prev_epoch[eid+1], w_iters[eid][chunkid], sm.sizePsi, 1.00/(no_of_mchns*1.00));

			}

			//Get the avg
			/*for(int i = 0; i < no_of_mchns; i++){
				add_vector_nn(w_prev_epoch[eid+1], w_avg[i], sm->sizePsi, 1/(no_of_mchns));
			}*/


			// After the completion of one epoch, warm start the 2nd epoch with the values of the
			// weight vectors seen at the end of the last chunk in previous epoch
			if(eid + 1 < totalEpochs){
				 //init w_iters[eid+1][0] to w_iters[eid][numChunks-1]
				 copy_vector(w_iters[eid+1][0], w_iters[eid][numChunks-1], sm.sizePsi);
				 printf("(ONLINE LEARNING) : WARM START ACROSS EPOCHS ..... DONE....\n");
			}

			printf("(OnlineSVM) : EPOCH COMPLETE -- primal objective: %.4f\n", primal_obj);
			printf("(ONLINE LEARNING) : EPOCH %d DONE! .....\n",eid);

			time(&time_end_full);
			char msg1[20];
			sprintf(msg1," EPOCH %d - Its All About Time! - ", eid);
			print_time(time_start_full, time_end_full, msg1);
			
			char *temp_model_file = malloc(1000);
			sprintf(temp_model_file,"%s_%d",modelfile,(eid+1));
			write_struct_model_online_distr(temp_model_file, w_prev_epoch[eid+1], &sparm, totalEpochs, numChunks);
			runTestCommand(temp_model_file);
			
//Sandip - Write model incremental
			/* write structural model */
//			 write_struct_model_online(modelfile, &sm, &sparm, (eid+1), numChunks);
//
//
//			char *result_stat_file = malloc(1000);
//			strcpy(result_stat_file, modelfile);
//			strcat(result_stat_file, ".result.stats");
//			FILE *fp = fopen(result_stat_file,"a");
//			if (fp==NULL) {
//				printf("Cannot open result stats file %s!\n", result_stat_file);
//				exit(1);
//			}
//
//			fprintf(fp,"\n\n---------------------------------------------------------------------------\n");
//			fprintf(fp,"Epoc No - %d      ", (eid+1));
//			fprintf(fp,"Chunks - %d\n", numChunks);
//			fprintf(fp,"---------------------------------------------------------------------------\n");
//			fclose(fp);
//			runTestCommand(modelfile);

		}

		time(&time_end_full);
		char msg[20];
		sprintf(msg,"(ONLINE LEARNING) : Total Time Taken : ");
		print_time(time_start_full, time_end_full, msg);

printf("(ONLINE LEARNING) Reached here\n");
/* write structural model */
//  write_struct_model_online(modelfile, &sm, &sparm, totalEpochs, numChunks);

//Sandip - Modifiedc for distr
	write_struct_model_online_distr(modelfile, w_prev_epoch[totalEpochs], &sparm, totalEpochs, numChunks);
	write_struct_model_online_distr_avg(strcat(modelfile,"_avg"), w_prev_epoch, &sparm, totalEpochs, numChunks);

	// skip testing for the moment
  printf("(ONLINE LEARNING) Complete dumping\n");

  /* free memory */ //TODO: Need to change this ...
  free_struct_sample(sample);
  free_struct_model(sm, &sparm);

  return(0); 
  
}

void print_time(time_t time_start, time_t time_end, char *msg){
	  double time_taken = (double)(time_end - time_start)/60;
	  printf("%s: %f mins\n", msg, time_taken);
	  fflush(stdout);
}


void my_read_input_parameters(int argc, char *argv[], char *trainfile, char* modelfile,
			      LEARN_PARM *learn_parm, KERNEL_PARM *kernel_parm, STRUCT_LEARN_PARM *struct_parm) {
  
  long i;

  /* set default */
  learn_parm->maxiter=20000;
  learn_parm->svm_maxqpsize=100;
  learn_parm->svm_c=100.0;
  //learn_parm->eps=0.001;
  learn_parm->eps=0.1; //AJAY: Changing for faster convergence
  learn_parm->biased_hyperplane=12345; /* store random seed */
  learn_parm->remove_inconsistent=10; 
  kernel_parm->kernel_type=0;
  kernel_parm->rbf_gamma=0.05;
  kernel_parm->coef_lin=1;
  kernel_parm->coef_const=1;
  kernel_parm->poly_degree=3;

  struct_parm->custom_argc=0;

  // Ajay
  learn_parm->totalEpochs = 1;
  learn_parm->numChunks = 5;
  learn_parm->machines = 5;

  // Loss types: (Note: normalisation when applied, is applied to both hamming and F1 if it is a combination loss)
  // 0 --> combination (Hamming + F1) + unnormalised
  // 1 --> only F1 + normalised (by number of chunked datasets)
  // 2 --> only Hamming + normalised (by number of chunked datasets)
  // 3 --> combination (Hamming + F1) + normalised (by number of chunked datasets)
  // 4 --> only F1 + unnormalised
  // 5 --> only Hamming + unnormalised
  learn_parm->lossType = 0;

  for(i=1;(i<argc) && ((argv[i])[0] == '-');i++) {
    switch ((argv[i])[1]) {
    case 'c': i++; learn_parm->svm_c=atof(argv[i]); break;
    case 'e': i++; learn_parm->eps=atof(argv[i]); break;
    case 's': i++; learn_parm->svm_maxqpsize=atol(argv[i]); break; 
    case 'g': i++; kernel_parm->rbf_gamma=atof(argv[i]); break;
    case 'd': i++; kernel_parm->poly_degree=atol(argv[i]); break;
    case 'r': i++; learn_parm->biased_hyperplane=atol(argv[i]); break; 
    case 't': i++; kernel_parm->kernel_type=atol(argv[i]); break;
    case 'n': i++; learn_parm->maxiter=atol(argv[i]); break;
    case 'p': i++; learn_parm->remove_inconsistent=atol(argv[i]); break;
    case '-': strcpy(struct_parm->custom_argv[struct_parm->custom_argc++],argv[i]);i++; strcpy(struct_parm->custom_argv[struct_parm->custom_argc++],argv[i]);break;
    // Added by Ajay
    case 'f': i++; strcpy(learn_parm->tmpdir,argv[i]); printf("\nTmp file is %s\n",learn_parm->tmpdir); break;
    case 'y': i++; learn_parm->frac_sim=atof(argv[i]); printf("Frac Sim is %g\n", learn_parm->frac_sim); break;
    case 'z': i++; strcpy(learn_parm->dataset_stats_file,argv[i]);printf("Dataset Stats file is %s\n",learn_parm->dataset_stats_file);break;
    case 'w': i++; learn_parm->Fweight=atof(argv[i]); printf("Weigting param of F is %g\n",learn_parm->Fweight);break;
    case 'o': i++; learn_parm->rho_admm=atof(argv[i]); printf("Rho is %g\n", learn_parm->rho_admm); break;
    case 'a': i++; learn_parm->isExhaustive=atol(argv[i]);printf("isExhaustive is %ld",learn_parm->isExhaustive); break;
    case 'b': i++; learn_parm->isLPrelaxation=atol(argv[i]);printf("isLPrelaxation is %ld",learn_parm->isLPrelaxation); break;
    case 'K': i++; learn_parm->numChunks=atoi(argv[i]); break;
    case 'E': i++; learn_parm->totalEpochs=atoi(argv[i]); break;
    case 'C': i++; learn_parm->Cdash=atof(argv[i]); break;
    case 'L': i++; learn_parm->lossType=atof(argv[i]); break;
    case 'M': i++; learn_parm->machines=atoi(argv[i]);break;
   ////////////////////////
    default: printf("\nUnrecognized option %s!\n\n",argv[i]);
      exit(0);
    }

  }

  if(i>=argc) {
    printf("\nNot enough input parameters!\n\n");
    my_wait_any_key();
    exit(0);
  }
  strcpy (trainfile, argv[i]);

  if((i+1)<argc) {
    strcpy (modelfile, argv[i+1]);
  }
  
  parse_struct_parameters(struct_parm);

}



void my_wait_any_key()
{
  printf("\n(more)\n");
  (void)getc(stdin);
}



int resize_cleanup(int size_active, int *idle, double *alpha, double *delta, double *gammaG0, double *proximal_rhs, double **G, DOC **dXc, double *cut_error) {
  int i,j, new_size_active;
  long k;

  i=0;
  while ((i<size_active)&&(idle[i]<IDLE_ITER)) i++;
  j=i;
  while((j<size_active)&&(idle[j]>=IDLE_ITER)) j++;

  while (j<size_active) {
    /* copying */
    alpha[i] = alpha[j];
    delta[i] = delta[j];
    gammaG0[i] = gammaG0[j];
    cut_error[i] = cut_error[j]; 
    
    free(G[i]);
    G[i] = G[j]; 
    G[j] = NULL;
    free_example(dXc[i],0);
    dXc[i] = dXc[j];
    dXc[j] = NULL;

    i++;
    j++;
    while((j<size_active)&&(idle[j]>=IDLE_ITER)) j++;
  }
  for (k=i;k<size_active;k++) {
    if (G[k]!=NULL) free(G[k]);
    if (dXc[k]!=NULL) free_example(dXc[k],0);
  }
  new_size_active = i;
  alpha = (double*)realloc(alpha, sizeof(double)*new_size_active);
  delta = (double*)realloc(delta, sizeof(double)*new_size_active);
  gammaG0 = (double*)realloc(gammaG0, sizeof(double)*new_size_active);
  proximal_rhs = (double*)realloc(proximal_rhs, sizeof(double)*new_size_active);
  G = (double**)realloc(G, sizeof(double*)*new_size_active);
  dXc = (DOC**)realloc(dXc, sizeof(DOC*)*new_size_active);
  cut_error = (double*)realloc(cut_error, sizeof(double)*new_size_active); 
  
  /* resize G and idle */
  i=0;
  while ((i<size_active)&&(idle[i]<IDLE_ITER)) i++;
  j=i;
  while((j<size_active)&&(idle[j]>=IDLE_ITER)) j++;

  while (j<size_active) {
    idle[i] = idle[j];
    for (k=0;k<new_size_active;k++) {
      G[k][i] = G[k][j];
    }
    i++;
    j++;
    while((j<size_active)&&(idle[j]>=IDLE_ITER)) j++;
  }  
  idle = (int*)realloc(idle, sizeof(int)*new_size_active);
  for (k=0;k<new_size_active;k++) {
    G[k] = (double*)realloc(G[k], sizeof(double)*new_size_active);
  }
  return(new_size_active);

}

