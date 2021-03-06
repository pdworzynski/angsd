/*

  The functionality of this file, has replaced the old emOptim and testfolded.c programs.

  part of ANGSD

  GNU license or whetever its called

  thorfinn@binf.ku.dk

  fixme: minor leaks in structures related to the thread structs, and the append function.
  
  Its july 13 2013, it is hot outside

  april 13, safv3 added, safv2 removed for know. Will be reintroduced later.
  april 20, removed 2dsfs as special scenario
  april 20, split out the safreader into seperate cpp/h
  may 5, seems to work well now
*/

#include <cstdio>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cfloat>
#include <signal.h>
#include <cassert>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <zlib.h>
#include <htslib/bgzf.h>
#include <htslib/tbx.h>

#ifdef __APPLE__
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

#include "safreader.h"
#include "keep.hpp"

#include "safstat.h"


double ttol = 1e-16; 
typedef struct {
  char *chooseChr;
  int start;
  int stop;
  int nSites;
  int maxIter;
  double tole;
  int nThreads;
  char *sfsfname;
  std::vector<persaf *> saf;
  int posOnly;
  char *fname;
  int onlyOnce;
  int emAccl;
}args;


int SIG_COND =1;
pthread_t *thd=NULL;
int howOften =5e6;//how often should we print out (just to make sure something is happening)
void destroy_safvec(std::vector<persaf *> &saf){
  //fprintf(stderr,"destroy &saf\n");
  for(int i=0;i<saf.size();i++)
    persaf_destroy(saf[i]);
}


void destroy_args(args *p){
  //  fprintf(stderr,"destroy args\n");
  destroy_safvec(p->saf);
  delete p;
}

//just approximate
template <typename T>
size_t fsizes(std::vector<persaf *> &pp, int nSites){
  size_t res = 0;
  for(int i=0;i<pp.size();i++){
    res += nSites*(pp[i]->nChr+1)*sizeof(T)+nSites*sizeof( T*);
  }
  return res;
}

size_t helper(persaf * pp,char *chr){
  if(chr==NULL)
    return pp->nSites;
  myMap::iterator it=pp->mm.find(chr);
  if(it==pp->mm.end()){
    fprintf(stderr,"\t-> Problem finding chromosome: %s\n",chr);
    exit(0);
  }
  return it->second.nSites;
}

size_t nsites(std::vector<persaf *> &pp,args *ar){
  if(ar->start!=-1 &&ar->stop!=-1)
    return ar->stop-ar->start;
  size_t res = helper(pp[0],ar->chooseChr);
  for(int i=1;i<pp.size();i++)
    if(helper(pp[i],ar->chooseChr) > res)
      res = helper(pp[i],ar->chooseChr);
  return res;
}


char * get_region(char *extra,int &start,int &stop) {
  if(!extra){
    fprintf(stderr,"Must supply parameter for -r option\n");
    return NULL;
  }
  if(strrchr(extra,':')==NULL){//only chromosomename
    char *ref = extra;
    start = stop = -1;;
    return ref;
  }
  char *tok=NULL;
  tok = strtok(extra,":");

  char *ref = tok;

  start =stop=-1;

  tok = extra+strlen(tok)+1;//tok now contains the rest of the string
 
  if(strlen(tok)==0)//not start and/or stop ex: chr21:
    return ref;
  

  if(tok[0]=='-'){//only contains stop ex: chr21:-stop
    tok =strtok(tok,"-");
    stop = atoi(tok);
  }else{
    //catch single point
    int isProper =0;
    for(size_t i=0;i<strlen(tok);i++)
      if(tok[i]=='-'){
	isProper=1;
	 break;
      }
    //fprintf(stderr,"isProper=%d\n",isProper);
    if(isProper){
      tok =strtok(tok,"-");
      start = atoi(tok)-1;//this is important for the zero offset
      tok = strtok(NULL,"-");
      if(tok!=NULL)
	stop = atoi(tok);
    }else{
      //single point
      stop = atoi(tok);
      start =stop -1;
      
    }
    
  }
  if(stop!=-1&&stop<start){
    fprintf(stderr,"endpoint:%d is larger than startpoint:%d\n",start,stop);
    exit(0);
    
  }
  if(0){
    fprintf(stderr,"[%s] ref=%s,start=%d,stop=%d\n",__FUNCTION__,ref,start,stop);
    exit(0);
  }
  return ref;
}




args * getArgs(int argc,char **argv){
  args *p = new args;

  p->sfsfname=p->chooseChr=NULL;
  p->start=p->stop=-1;
  p->maxIter=1e2;
  p->tole=1e-6;
  p->nThreads=4;
  p->nSites =0;
  p->posOnly = 0;
  p->fname = NULL;
  p->onlyOnce = 0;
  p->emAccl =1;
  if(argc==0)
    return p;

  while(*argv){
    //    fprintf(stderr,"%s\n",*argv);
    if(!strcasecmp(*argv,"-tole"))
      p->tole = atof(*(++argv));
    else  if(!strcasecmp(*argv,"-P"))
      p->nThreads = atoi(*(++argv));
    else  if(!strcasecmp(*argv,"-maxIter"))
      p->maxIter = atoi(*(++argv));
    else  if(!strcasecmp(*argv,"-posOnly"))
      p->posOnly = atoi(*(++argv));
    else  if(!strcasecmp(*argv,"-nSites"))
      p->nSites = atoi(*(++argv));
    else  if(!strcasecmp(*argv,"-m"))
      p->emAccl = atoi(*(++argv));

    else  if(!strcasecmp(*argv,"-onlyOnce"))
      p->onlyOnce = atoi(*(++argv));
    else  if(!strcasecmp(*argv,"-r")){
      p->chooseChr = get_region(*(++argv),p->start,p->stop);
      if(!p->chooseChr)
	return NULL;
    }
    else  if(!strcasecmp(*argv,"-start")){
      p->sfsfname = *(++argv);
    }else{
      p->saf.push_back(persaf_init<float>(*argv));
      p->fname = *argv;
      //   fprintf(stderr,"toKeep:%p\n",p->saf[p->saf.size()-1]->toKeep);
    }
    argv++;
  }

  fprintf(stderr,"\t-> args: tole:%f nthreads:%d maxiter:%d nsites:%d start:%s chr:%s start:%d stop:%d fname:%s\n",p->tole,p->nThreads,p->maxIter,p->nSites,p->sfsfname,p->chooseChr,p->start,p->stop,p->fname);
  return p;
}

int printOld(int argc,char **argv){

  if(argc<1){
    fprintf(stderr,"Must supply afile.saf.idx [chrname]\n");
    return 0; 
  }
  
  args *pars = getArgs(argc,argv);
  if(!pars)
    return 1;
  if(pars->saf.size()!=1){
    fprintf(stderr,"Print only implemeted for single safs\n");
    exit(0);
  }
  writesaf_header(stderr,pars->saf[0]);
  
  float *flt = new float[pars->saf[0]->nChr+1];
  for(myMap::iterator it=pars->saf[0]->mm.begin();it!=pars->saf[0]->mm.end();++it){
    if(pars->chooseChr!=NULL){
      it = pars->saf[0]->mm.find(pars->chooseChr);
      if(it==pars->saf[0]->mm.end()){
	fprintf(stderr,"Problem finding chr: %s\n",pars->chooseChr);
	break;
      }
    }
    bgzf_seek(pars->saf[0]->pos,it->second.pos,SEEK_SET);
    bgzf_seek(pars->saf[0]->saf,it->second.saf,SEEK_SET);
    int *ppos = new int[it->second.nSites];
    bgzf_read(pars->saf[0]->pos,ppos,sizeof(int)*it->second.nSites);
    
    int first=0;
    if(pars->start!=-1)
      while(ppos[first]<pars->start) 
	first++;
    
    int last=it->second.nSites;
    //    fprintf(stderr,"pars-.stop:%d ppos:%d\n",pars->stop,ppos[last-1]);
    if(pars->stop!=-1&&pars->stop<=ppos[last-1]){
      last=first;
      while(ppos[last]<pars->stop) 
	last++;
    }
    //fprintf(stderr,"first:%d last:%d\n",first,last);
    int at=0;
    for(int s=0;SIG_COND&&s<it->second.nSites;s++) {
      bgzf_read(pars->saf[0]->saf,flt,sizeof(float)*(pars->saf[0]->nChr+1));
      if(at>=first&&at<last){
	if(pars->posOnly==0){
	  fprintf(stdout,"%s\t%d",it->first,ppos[s]+1);
	  for(int is=0;is<pars->saf[0]->nChr+1;is++)
	    fprintf(stdout,"\t%f",flt[is]);
	}else
	  fprintf(stdout,"%d",ppos[s]+1);
	  fprintf(stdout,"\n");
      }
      at++;
    }
    delete [] ppos;
    if(pars->chooseChr!=NULL)
      break;
  }
  
  delete [] flt;
  destroy_args(pars);
  return 0;
}

void print(int argc,char **argv){
  if(argc<1){
    fprintf(stderr,"Must supply afile.saf.idx \n");
    return; 
  }
  
  args *pars = getArgs(argc,argv);
  pars->saf[0]->kind = 2;
  if(pars->posOnly==1)
    pars->saf[0]->kind = 1;
  if(pars->saf.size()!=1){
    fprintf(stderr,"Print only implemeted for single safs\n");
    exit(0);
  }

  writesaf_header(stderr,pars->saf[0]);
  
  float *flt = new float[pars->saf[0]->nChr+1];
  for(myMap::iterator it=pars->saf[0]->mm.begin();it!=pars->saf[0]->mm.end();++it){

    if(pars->chooseChr!=NULL)
      it = iter_init(pars->saf[0],pars->chooseChr,pars->start,pars->stop);
    else
      it = iter_init(pars->saf[0],it->first,pars->start,pars->stop);
 
    int ret;
    int pos;

    while((ret=iter_read(pars->saf[0],flt,sizeof(float)*(pars->saf[0]->nChr+1),&pos))){
      fprintf(stdout,"%s\t%d",it->first,pos+1);
      for(int is=0;pars->posOnly!=1&&is<pars->saf[0]->nChr+1;is++)
	fprintf(stdout,"\t%f",flt[is]);
      fprintf(stdout,"\n");
    }
 
    if(pars->chooseChr!=NULL)
      break;
  }
  
  delete [] flt;
  destroy_args(pars);
}


#if 0
int index(int argc,char **argv){
  fprintf(stderr,"tabix not implemented yet\n");
  if(argc<1){
    fprintf(stderr,"Must supply afile.saf.idx \n");
    return -1; 
  }
  
  args *pars = getArgs(argc,argv);
  
  if(pars->saf.size()!=1){
    fprintf(stderr,"Print only implemeted for single safs\n");
    exit(0);
  }

  //  writesaf_header(stderr,pars->saf[0]);

  BGZF *fp=pars->saf[0]->saf;
  if ( !fp->is_compressed ) {
    bgzf_close(fp);
    fprintf(stderr,"not compressed\n") ;
    return -1; 
  }

  tbx_t *tbx = (tbx_t*)calloc(1, sizeof(tbx_t));
  int min_shift = 14;
  int n_lvls = 5;
  int fmt = HTS_FMT_TBI;


  float *flt = new float[pars->saf[0]->nChr+1];
  for(myMap::iterator it=pars->saf[0]->mm.begin();it!=pars->saf[0]->mm.end();++it){
    int *ppos = new int [it->second.nSites];
    bgzf_seek(pars->saf[0]->pos,it->second.pos,SEEK_SET);
    bgzf_read(pars->saf[0]->pos,ppos,sizeof(int)*it->second.nSites);

    int ret;
    fprintf(stderr,"in print2 first:%lu last:%lu\n",pars->saf[0]->toKeep->first,pars->saf[0]->toKeep->last);
    int pos;
    while((ret=iter_read(pars->saf[0],flt,sizeof(float)*(pars->saf[0]->nChr+1),&pos))){
   
      //      fprintf(stderr,"[%s] pars->saf[0]->at:%d nSites: %lu ret:%d\n",__FUNCTION__,pars->saf[0]->at,it->second.nSites,ret);
      fprintf(stdout,"%s\t%d",it->first,ppos[pars->saf[0]->at]+1);
      for(int is=0;is<pars->saf[0]->nChr+1;is++)
	fprintf(stdout,"\t%f",flt[is]);
      fprintf(stdout,"\n");
    }
    delete [] ppos;
    //fprintf(stderr,"[%s] after while:%d\n",__FUNCTION__,ret);
    if(pars->chooseChr!=NULL)
      break;
  }
  
  delete [] flt;
  destroy_args(pars);

  const char *suffix = ".tbi";
  char *idx_fname =(char*) calloc(strlen(pars->fname) + 5, 1);
  strcat(strcpy(idx_fname, pars->fname), suffix);
  fprintf(stderr,"idx_fname:%s\n",idx_fname);


}
#endif


template<typename T>
struct emPars{
  int threadId;
  std::vector <Matrix<T> *> gls;
  int from;
  int to;
  double lik;
  double *sfs;//shared for all threads
  double *post;//allocated for every thread
  int dim;
};



emPars<float> *emp = NULL;




void normalize(double *tmp,int len){
  double s=0;
  for(int i=0;i<len;i++)
    s += tmp[i];
  for(int i=0;i<len;i++)
    tmp[i] /=s;
}



#ifdef __APPLE__
size_t getTotalSystemMemory(){
  uint64_t mem;
  size_t len = sizeof(mem);
  sysctlbyname("hw.memsize", &mem, &len, NULL, 0);
  return mem;
}
#else
size_t getTotalSystemMemory(){
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    return pages * page_size;
}
#endif

template<typename T>
void readGL(persaf *fp,size_t nSites,int dim,Matrix<T> *ret){

  // ret->x=nSites;
  ret->y=dim;
  size_t i;
  for(i=ret->x;SIG_COND&&i<nSites;i++){
    if(i>0 &&(i% howOften)==0  )
      fprintf(stderr,"\r\t-> Has read 5mio sites now at: %lu      ",i);
    //
    int pos;
    int bytes_read= iter_read(fp,ret->mat[i],sizeof(T)*dim,&pos);//bgzf_read(fp,ret->mat[i],sizeof(T)*dim);
    if(bytes_read!=0 && bytes_read<sizeof(T)*dim){
      fprintf(stderr,"Problem reading chunk from file, please check nChr is correct, will exit \n");
      exit(0);
    }
    if(bytes_read==0){
      //fprintf(stderr,"[%s] bytes_read==0 at i:%lu\n",__FUNCTION__,i);
      break;
    }
    for(size_t j=0;j<dim;j++)
      ret->mat[i][j] = exp(ret->mat[i][j]);
  }
  //fprintf(stderr,"[%s] i:%lu\n",__FUNCTION__,i);
  ret->x=i;
  if(SIG_COND==0)
    exit(0);
  //  matrix_print<T>(ret);
  //  exit(0);
  fprintf(stderr,"\r");
}

//returns the number of sites read
template<typename T>
int readGLS(std::vector<persaf *> &adolf,size_t nSites,std::vector< Matrix<T> *> &ret){
  int pre=ret[0]->x;
  for(int i=0;i<adolf.size();i++){
    readGL(adolf[i],nSites,adolf[i]->nChr+1,ret[i]);
    //    fprintf(stderr,"adolf:%d\t%lu\n",i,ret[i]->x);
  }
  
  return ret[0]->x-pre;
}


size_t fsize(const char* fname){
  struct stat st ;
  stat(fname,&st);
  return st.st_size;
}

void readSFS(const char*fname,int hint,double *ret){
  fprintf(stderr,"\t-> Reading: %s assuming counts (will normalize to probs internally)\n",fname);
  FILE *fp = NULL;
  if(((fp=fopen(fname,"r")))==NULL){
    fprintf(stderr,"problems opening file:%s\n",fname);
    exit(0);
  }
  char buf[fsize(fname)+1];
  if(fsize(fname)!=fread(buf,sizeof(char),fsize(fname),fp)){
    fprintf(stderr,"Problems reading file: %s\n will exit\n",fname);
    exit(0);
  }
  buf[fsize(fname)]='\0';
  std::vector<double> res;
  char *tok=NULL;
  tok = strtok(buf,"\t\n ");
  if(!tok){
    fprintf(stderr,"File:%s looks empty\n",fname);
    exit(0);
  }
  res.push_back(atof(tok));

  while((tok=strtok(NULL,"\t\n "))) {  
    //fprintf(stderr,"%s\n",tok);
    res.push_back(atof(tok));

  }
  //  fprintf(stderr,"size of prior=%lu\n",res.size());
  if(hint!=res.size()){
    fprintf(stderr,"problem with size of dimension of prior %d vs %lu\n",hint,res.size());
    for(size_t i=0;0&&i<res.size();i++)
      fprintf(stderr,"%zu=%f\n",i,res[i]);
    exit(0);
  }
  for(size_t i=0;i<res.size();i++){
      ret[i] = res[i];
      //      fprintf(stderr,"i=%lu %f\n",i,ret[i]);
  }
  normalize(ret,res.size());
  fclose(fp);
}

template <typename T>
double lik1(double *sfs,std::vector< Matrix<T> *> &gls,int from,int to){
  //  fprintf(stderr,"[%s] from:%d to:%d\n",__FUNCTION__,from,to);
  double r =0;
  for(int s=from;s<to;s++){
    double tmp =0;
    for(int i=0;i<gls[0]->y;i++)
      tmp += sfs[i]* gls[0]->mat[s][i];
    r += log(tmp);
  }
  return r;
}
template <typename T>
double lik2(double *sfs,std::vector< Matrix<T> *> &gls,int from,int to){
  double r =0;
  for(int s=from;s<to;s++){
    double tmp =0;
    int inc =0;
    for(int i=0;i<gls[0]->y;i++)
      for(int j=0;j<gls[1]->y;j++)
	tmp += sfs[inc++]* gls[0]->mat[s][i] *gls[1]->mat[s][j];
    r += log(tmp);
  }
  return r;
}
template <typename T>
double lik3(double *sfs,std::vector< Matrix<T> *> &gls,int from,int to){
  double r =0;
  for(int s=from;s<to;s++){
    double tmp =0;
    int inc =0;
    for(int i=0;i<gls[0]->y;i++)
      for(int j=0;j<gls[1]->y;j++)
	for(int k=0;k<gls[2]->y;k++)
	tmp += sfs[inc++]* gls[0]->mat[s][i] *gls[1]->mat[s][j]*gls[2]->mat[s][k];
    r += log(tmp);
  }
  return r;
}
template <typename T>
double lik4(double *sfs,std::vector< Matrix<T> *> &gls,int from,int to){
  double r =0;
  for(int s=from;s<to;s++){
    double tmp =0;
    int inc =0;
    for(int i=0;i<gls[0]->y;i++)
      for(int j=0;j<gls[1]->y;j++)
	for(int k=0;k<gls[2]->y;k++)
	  for(int m=0;m<gls[3]->y;m++)
	tmp += sfs[inc++]* gls[0]->mat[s][i] *gls[1]->mat[s][j]*gls[2]->mat[s][k]*gls[3]->mat[s][m];
    r += log(tmp);
  }
  return r;
}

template <typename T>
void *like_slave(void *p){
  emPars<T> &pars = emp[(size_t) p];
  if(pars.gls.size()==1)
    pars.lik = lik1(pars.sfs,pars.gls,pars.from,pars.to);
  else if(pars.gls.size()==2)
    pars.lik = lik2(pars.sfs,pars.gls,pars.from,pars.to);
  else if(pars.gls.size()==3)
    pars.lik = lik3(pars.sfs,pars.gls,pars.from,pars.to);
  else if(pars.gls.size()==4)
    pars.lik = lik4(pars.sfs,pars.gls,pars.from,pars.to);
  
  pthread_exit(NULL);
}


template <typename T>
double like_master(int nThreads){
  for(size_t i=0;i<nThreads;i++){
    int rc = pthread_create(&thd[i],NULL,like_slave<T>,(void*) i);
    if(rc)
      fprintf(stderr,"Error creating thread\n");
    
  }
  for(int i=0;i<nThreads;i++)
    pthread_join(thd[i], NULL);
    
  double res=0;
  for(int i=0;i<nThreads;i++){
    //    fprintf(stderr,"lik=%f\n",emp[i].lik);
    res += emp[i].lik;
  
  }
  return res;
}



template <typename T>
void emStep1(double *pre,std::vector< Matrix<T> * > &gls,double *post,int start,int stop,int dim){
  double inner[dim];
  for(int x=0;x<dim;x++)
    post[x] =0.0;
    
  for(int s=start;SIG_COND&&s<stop;s++){
    for(int x=0;x<dim;x++)
      inner[x] = pre[x]*gls[0]->mat[s][x];
  
   normalize(inner,dim);
   for(int x=0;x<dim;x++)
     post[x] += inner[x];
  }
  normalize(post,dim);
 
}


template <typename T>
void emStep2(double *pre,std::vector<Matrix<T> *> &gls,double *post,int start,int stop,int dim){
  double inner[dim];
  for(int x=0;x<dim;x++)
    post[x] =0.0;
    
  for(int s=start;SIG_COND&&s<stop;s++){
    int inc=0;
    for(int x=0;x<gls[0]->y;x++)
      for(int y=0;y<gls[1]->y;y++){
	inner[inc] = pre[inc]*gls[0]->mat[s][x]*gls[1]->mat[s][y];
	inc++;
      }
   normalize(inner,dim);
   for(int x=0;x<dim;x++)
     post[x] += inner[x];
  }
  normalize(post,dim);
 
}

template <typename T>
void emStep3(double *pre,std::vector<Matrix<T> *> &gls,double *post,int start,int stop,int dim){
  double inner[dim];
  for(int x=0;x<dim;x++)
    post[x] =0.0;
    
  for(int s=start;SIG_COND&&s<stop;s++){
    int inc=0;
    for(int x=0;x<gls[0]->y;x++)
      for(int y=0;y<gls[1]->y;y++)
	for(int i=0;i<gls[2]->y;i++){
	  inner[inc] = pre[inc]*gls[0]->mat[s][x] * gls[1]->mat[s][y] * gls[2]->mat[s][i];
	  inc++;
	}
   normalize(inner,dim);
   for(int x=0;x<dim;x++)
     post[x] += inner[x];
  }
  normalize(post,dim);
   
}

template <typename T>
void emStep4(double *pre,std::vector<Matrix<T> *> &gls,double *post,int start,int stop,int dim){
  double inner[dim];
  for(int x=0;x<dim;x++)
    post[x] =0.0;
    
  for(int s=start;SIG_COND&&s<stop;s++){
    int inc=0;
    for(int x=0;x<gls[0]->y;x++)
      for(int y=0;y<gls[1]->y;y++)
	for(int i=0;i<gls[2]->y;i++)
	  for(int j=0;j<gls[3]->y;j++){
	    inner[inc] = pre[inc]*gls[0]->mat[s][x] * gls[1]->mat[s][y] * gls[2]->mat[s][i]* gls[3]->mat[s][j];
	    inc++;
	  }

  }
  normalize(inner,dim);
  for(int x=0;x<dim;x++)
    post[x] += inner[x];
  
  normalize(post,dim);
   
}

template <typename T>
void *emStep_slave(void *p){
  emPars<T> &pars = emp[(size_t) p];
  if(pars.gls.size()==1)
    emStep1<T>(pars.sfs,pars.gls,pars.post,pars.from,pars.to,pars.dim);
  else if(pars.gls.size()==2)
    emStep2<T>(pars.sfs,pars.gls,pars.post,pars.from,pars.to,pars.dim);
  else if(pars.gls.size()==3)
    emStep3<T>(pars.sfs,pars.gls,pars.post,pars.from,pars.to,pars.dim);
  else if(pars.gls.size()==4)
    emStep4<T>(pars.sfs,pars.gls,pars.post,pars.from,pars.to,pars.dim);
  pthread_exit(NULL);
}


template<typename T>
void emStep_master(double *post,int nThreads){
  for(size_t i=0;i<nThreads;i++){
    int rc = pthread_create(&thd[i],NULL,emStep_slave<T>,(void*) i);
    if(rc)
      fprintf(stderr,"Error creating thread\n");
    
  }
  for(int i=0;i<nThreads;i++)
    pthread_join(thd[i], NULL);
    
  memcpy(post,emp[0].post,emp[0].dim*sizeof(double));
  for(int i=1;i<nThreads;i++){
    for(int j=0;j<emp[0].dim;j++)
      post[j] += emp[i].post[j];
  }
  
  normalize(post,emp[0].dim);

#if 0
  for(int i=0;i<nThreads;i++){
    for(int j=0;j<dim;j++)
      fprintf(stdout,"%f ",emp[i].post[j]);
    fprintf(stdout,"\n");
  }
#endif
  
}


template<typename T>
emPars<T> *setThreadPars(std::vector<Matrix<T> * > &gls,double *sfs,int nThreads,int dim,int nSites){
  //  fprintf(stderr,"nSites:%d\n",nSites);
  emPars<T> *temp = new emPars<T>[nThreads];
  int blockSize = nSites/nThreads;
  for(int i=0;i<nThreads;i++){
    temp[i].threadId = i;
    temp[i].gls=gls;
    temp[i].from =0;
    temp[i].to=blockSize;
    temp[i].sfs = sfs;
    temp[i].post=new double[dim];
    temp[i].dim = dim;
  }
  //redo the from,to
  for(int i=1;i<nThreads;i++){
    temp[i].from = temp[i-1].to;
    temp[i].to = temp[i].from+blockSize;
  }
  //fix last end point
  temp[nThreads-1].to=nSites;
#if 0
  fprintf(stderr,"--------------\n");
  for(int i=0;i<nThreads;i++)
    fprintf(stderr,"threadinfo %d)=(%d,%d)=%d \n",temp[i].threadId,temp[i].from,temp[i].to,temp[i].to-temp[i].from); //
  fprintf(stderr,"--------------\n");
#endif 
  
  thd= new pthread_t[nThreads];
  return temp;
}

template<typename T>
void destroy(emPars<T> *a,int nThreads ){
  for(int i=0;i<nThreads;i++)
    delete [] a[i].post;
  delete [] a;
  delete [] thd;
}



template <typename T>
double em(double *sfs,double tole,int maxIter,int nThreads,int dim,std::vector<Matrix<T> *> &gls){
  emp = setThreadPars<T>(gls,sfs,nThreads,dim,gls[0]->x);
  fprintf(stderr,"------------\n");
  double oldLik,lik;
  oldLik = like_master<T>(nThreads);
  
  fprintf(stderr,"startlik=%f\n",oldLik);fflush(stderr);
  double tmp[dim];
  
  for(int it=0;SIG_COND&&it<maxIter;it++) {
    emStep_master<T>(tmp,nThreads);
    
    for(int i=0;i<dim;i++)
      sfs[i]= tmp[i];

    lik = like_master<T>(nThreads);

    fprintf(stderr,"[%d] lik=%f diff=%g\n",it,lik,fabs(lik-oldLik));

    if(fabs(lik-oldLik)<tole){
      oldLik=lik;
      break;
    }
    oldLik=lik;
  }
destroy<T>(emp,nThreads);
  return oldLik;
}


template <typename T>
double emAccl(double *p,double tole,int maxIter,int nThreads,int dim,std::vector<Matrix<T> *> &gls,int useSq){
  emp = setThreadPars<T>(gls,p,nThreads,dim,gls[0]->x);
  fprintf(stderr,"------------\n");
  double oldLik,lik;
  oldLik = like_master<T>(nThreads);
  
  fprintf(stderr,"startlik=%f\n",oldLik);fflush(stderr);
  double p1[dim];
  double p2[dim];
  double q1[dim];
  double q2[dim];
  double pnew[dim];
  int stepMax = 1;
  int mstep = 4;
  int stepMin = 1;
  
  int iter =0;

  while(SIG_COND&&iter<maxIter){
    emStep_master<T>(p1,nThreads);
    iter++;
    double sr2 =0;
    for(int i=0;i<dim;i++){
      q1[i] = p1[i]-p[i];
      sr2 += q1[i]*q1[i];
    }
    double oldp[dim];
    memcpy(oldp,p,sizeof(double)*dim);

    memcpy(p,p1,sizeof(double)*dim);  
    if(sqrt(sr2)<tole){
      fprintf(stderr,"breaking sr2 at iter:%d\n",iter);
      break;
    }
    emStep_master<T>(p2,nThreads);
    iter++;


    double sq2 =0;
    for(int i=0;i<dim;i++){
      q2[i] = p2[i]-p1[i];
      sq2 += q2[i]*q2[i];
    }

    if(sqrt(sq2)<tole){
      fprintf(stderr,"breaking sq2 at iter:%d\n",iter);
      break;
    }
    double sv2=0;
    for(int i=0;i<dim;i++)
      sv2 += (q2[i]-q1[i])*(q2[i]-q1[i]);


    double alpha = sqrt(sr2/sv2);
    alpha =std::max(stepMin*1.0,std::min(1.0*stepMax,alpha));
    
    //the magical step
    for(int j=0;j<dim;j++)
      pnew[j] = oldp[j] + 2.0 * alpha * q1[j] + alpha*alpha * (q2[j] - q1[j]);

#if 1 //fix for going out of bound
    for(int j=0;j<dim;j++){
      if(pnew[j]<ttol)
	pnew[j]=ttol;
      if(pnew[j]>1-ttol)
	pnew[j]=1-ttol;
    }
    normalize(pnew,dim);
#endif
    if(fabs(alpha-1) >0.01){
      //this is clearly to stabilize
      double tmp[dim];
      memcpy(p,pnew,sizeof(double)*dim);
      emStep_master<T>(tmp,nThreads);
      memcpy(pnew,tmp,sizeof(double)*dim);
      iter++;
    }
    memcpy(p,pnew,sizeof(double)*dim);
    

    //like_master is using sfs[] to calculate like
    lik = like_master<T>(nThreads);
    fprintf(stderr,"lik[%d]=%f diff=%g stepMax:%d stepMin:%d\n",iter,lik,fabs(lik-oldLik),stepMax,stepMin);
    if(std::isnan(lik)) {
      fprintf(stderr,"\t-> Observed NaN in accelerated EM, will use last reliable value. Consider using as input for ordinary em\n");
      fprintf(stderr,"\t-> E.g ./realSFS -start current.output -m 0 >new.output\n");//thanks morten rasmussen
      memcpy(p,p2,sizeof(double)*dim);
      break;
    }

    
    if(0&&lik<oldLik)//this should at some point be investigated further //
      fprintf(stderr,"\t-> New like is worse?\n");
#if 1
    if(fabs(lik-oldLik)<tole){
      oldLik=lik;
      break;
    }
    oldLik=lik;
#endif
    if (alpha == stepMax) 
      stepMax = mstep * stepMax;
    if(stepMin<0 &&alpha==stepMin)
      stepMin = mstep*stepMin;
    
  }
  destroy<T>(emp,nThreads);
  return oldLik;
}


int really_kill =3;
int VERBOSE = 1;
void handler(int s) {
  if(s==13)//this is sigpipe
    exit(0);
  if(VERBOSE)
    fprintf(stderr,"\n\t-> Caught SIGNAL: Will try to exit nicely (no more threads are created.\n\t\t\t  We will wait for the current threads to finish)\n");
  
  if(--really_kill!=3)
  fprintf(stderr,"\n\t-> If you really want \'realSFS\' to exit uncleanly ctrl+c: %d more times\n",really_kill+1);
  fflush(stderr);
  if(!really_kill)
    exit(0);
  VERBOSE=0;
  SIG_COND=0;

}


size_t parspace(std::vector<persaf *> &saf){
  size_t ndim = 1;
  for(int i=0;i<saf.size();i++)
    ndim *= saf[i]->nChr+1;
  fprintf(stderr,"\t-> Dimension of parameter space: %lu\n",ndim);
  return ndim;
}

//unthreaded
//this will populate the keep vector by
// 1) set the chooseChr and populate toKeep
// 2) find over lap between different positions
// this is run once for each chromsome
int set_intersect_pos(std::vector<persaf *> &saf,char *chooseChr,int start,int stop){
  //  fprintf(stderr,"[%s] chooseChr:%s, start:%d stop:%d\n",__FUNCTION__,chooseChr,start,stop );

  if(saf.size()==1&&chooseChr==NULL){//use entire genome, then don't do any strange filtering
    //fprintf(stderr,"herer\n");
    return 0 ;
  }
  /*
    What happens here? very good question

   */
  static int firstTime =1;
  static myMap::iterator it_outer=saf[0]->mm.begin();
  if(chooseChr==NULL){
    if(it_outer==saf[0]->mm.end())//if we are done
      return -2;
    else if(firstTime==0){
      it_outer++;
      if(it_outer==saf[0]->mm.end()){
	//	fprintf(stderr,"done reading will exit \n");
	return -3;
      }
    }else
      firstTime =0;
    chooseChr = it_outer->first;
    fprintf(stderr,"\t-> Is in multi sfs, will now read data from chr:%s\n",chooseChr);
  }
  
  fprintf(stderr,"\t-> hello Im the master merge part of realSFS. and I'll now do a tripple bypass to find intersect \n");
  fprintf(stderr,"\t-> 1) Will set iter according to chooseChr and start and stop\n");
  assert(chooseChr!=NULL);

 //hit will contain the depth for the different populations
  keep<char> *hit =NULL;
  //  assert(saf.size()>1);

  if(saf.size()>1)
    hit =keep_alloc<char>();//
  
  
  //this loop will populate a 'hit' array containing the effective (differnt pops) depth
  //if we only have one population, then just return after iter_init
  for(int i=0;i<saf.size();i++){
    myMap::iterator it = iter_init(saf[i],chooseChr,start,stop);
    assert(it!=saf[i]->mm.end());  
    if(saf.size()==1)
      return 0;
    
    bgzf_seek(saf[i]->pos,it->second.pos,SEEK_SET);
    saf[i]->ppos = new int[it->second.nSites];
    bgzf_read(saf[i]->pos,saf[i]->ppos,it->second.nSites*sizeof(int));
    if(saf[i]->ppos[it->second.nSites-1] > hit->m)
      realloc(hit,saf[i]->ppos[it->second.nSites-1]+1);
    assert(hit->m>0);
    //    fprintf(stderr,"keep[%d].first:%lu last:%lu\n",i,saf[i]->toKeep->first,saf[i]->toKeep->last);
    for(int j=saf[i]->toKeep->first;j<saf[i]->toKeep->last;j++)
      if(saf[i]->toKeep->d[j])
	hit->d[saf[i]->ppos[j]]++;
  }
#if 0
  keep_info(hit,stderr,0,saf.size());
  for(int i=0;0&i<hit->m;i++)
    if(hit->d[i]==saf.size())
      fprintf(stdout,"%d\n",i);
  exit(0);
#endif
  //hit now contains the genomic position (that is the index).
  
  //let us now modify the the persaf toKeep char vector
  int tsk[saf.size()];
  for(int i=0;i<saf.size();i++){
    tsk[i] =0;
    for(int j=0;j<saf[i]->toKeep->last;j++)
      if(hit->d[saf[i]->ppos[j]]!=saf.size())
	saf[i]->toKeep->d[j] =0;
      else
	tsk[i]++;
    fprintf(stderr,"\t-> Sites to keep from pop%d:\t%d\n",i,tsk[i]);
    if(i>0)
      assert(tsk[i]==tsk[i-1]);
#if 0
    keep_info(saf[i]->toKeep,stderr,0,1);
    //print out overlapping posiitons for all pops
    
    for(int j=0;j<saf[i]->toKeep->last;j++){
      if(hit->d[saf[i]->ppos[j]]==saf.size())
	fprintf(stdout,"saf%d\t%d\n",i,j);
    }
#endif
  }
  //  exit(0);
  keep_destroy(hit);
}
/*
  return value 
  -3 indicates that we are doing multi sfs and that we are totally and should flush

 */



template <typename T>
int readdata(std::vector<persaf *> &saf,std::vector<Matrix<T> *> &gls,int nSites,char *chooseChr,int start,int stop){

  static int lastread=0;
  //  fprintf(stderr,"[%s] nSites:%d lastread:%d\n",__FUNCTION__,nSites,lastread);
  if(lastread==0 ){
    //    fprintf(stderr,"\t-> Done reading data from chromosome will prepare next chromosome\n");
    int ret = set_intersect_pos(saf,chooseChr,start,stop); 
    //fprintf(stderr,"ret:%d\n",ret);
    if(ret==-3)
      return -3;
  }
  lastread=readGLS(saf,nSites,gls);
  if(lastread==0)
    fprintf(stderr,"\t-> Only read nSites: %lu will therefore prepare next chromosome (or exit)\n",gls[0]->x);
  //fprintf(stderr,"readdata lastread:%d\n\n",lastread);
  // exit(0);
  if(chooseChr!=NULL&&lastread==0){
    //fprintf(stderr,"return -2\n");
    return -2;
  }
  else if(chooseChr==NULL &&lastread==0 )
    return -2;
  else
    return 1;
}


template <typename T>
int main_opt(args *arg){
  std::vector<persaf *> &saf =arg->saf;
  int nSites = arg->nSites;
  if(nSites == 0){//if no -nSites is specified
    nSites=nsites(saf,arg);
  }
  if(fsizes<T>(saf,nSites)>getTotalSystemMemory())
    fprintf(stderr,"\t-> Looks like you will allocate too much memory, consider starting the program with a lower -nSites argument\n"); 
    
  fprintf(stderr,"\t-> nSites: %d\n",nSites);
  float bytes_req_megs = fsizes<T>(saf,nSites)/1024/1024;
  float mem_avail_megs = getTotalSystemMemory()/1024/1024;//in percentile
  //fprintf(stderr,"en:%zu to:%f\n",bytes_req_megs,mem_avail_megs);
  fprintf(stderr,"\t-> The choice of -nSites will require atleast: %f megabyte memory, that is at least: %.2f%% of total memory\n",bytes_req_megs,bytes_req_megs*100/mem_avail_megs);

  std::vector<Matrix<T> *> gls;
  for(int i=0;i<saf.size();i++)
    gls.push_back(alloc<T>(nSites,saf[i]->nChr+1));

  int ndim= parspace(saf);
  double *sfs=new double[ndim];
  
  
  while(1) {
    int ret=readdata(saf,gls,nSites,arg->chooseChr,arg->start,arg->stop);//read nsites from data
    //    fprintf(stderr,"\t\tRET:%d\n",ret);
    if(ret==-2&gls[0]->x==0)//no more data in files or in chr, eith way we break;
      break;
    
    if(saf.size()==1){
      if(ret!=-2){
	if(gls[0]->x!=nSites&&arg->chooseChr==NULL&&ret!=-3){
	  //	  fprintf(stderr,"continue continue\n");
	  continue;
	}
      }
    }else{
      if(gls[0]->x!=nSites&&arg->chooseChr==NULL&&ret!=-3){
	//fprintf(stderr,"continue continue\n");
	continue;
      }

    }
  
      
    fprintf(stderr,"\t-> Will run optimization on nSites: %lu\n",gls[0]->x);
    
    if(arg->sfsfname!=NULL)
      readSFS(arg->sfsfname,ndim,sfs);
    else
      for(int i=0;i<ndim;i++)
	sfs[i] = (i+1)/((double)(ndim));

    normalize(sfs,ndim);

    
    double lik;
    if(arg->emAccl==0)
      lik = em<float>(sfs,arg->tole,arg->maxIter,arg->nThreads,ndim,gls);
    else
      lik = emAccl<float>(sfs,arg->tole,arg->maxIter,arg->nThreads,ndim,gls,arg->emAccl);
    fprintf(stderr,"likelihood: %f\n",lik);
    fprintf(stderr,"------------\n");
#if 1
    //    fprintf(stdout,"#### Estimate of the sfs ####\n");
    for(int x=0;x<ndim;x++)
      fprintf(stdout,"%f ",gls[0]->x*sfs[x]);
    fprintf(stdout,"\n");
    fflush(stdout);
#endif
    
    for(int i=0;i<gls.size();i++)
      gls[i]->x =0;
    
    if(ret==-2&&arg->chooseChr!=NULL)
      break;
    if(arg->onlyOnce)
      break;
  }

  destroy(gls,nSites);
  destroy_args(arg);
  delete [] sfs;
  
  fprintf(stderr,"\n\t-> NB NB output is no longer log probs of the frequency spectrum!\n");
  fprintf(stderr,"\t-> Output is now simply the expected values! \n");
  fprintf(stderr,"\t-> You can convert to the old format simply with log(norm(x))\n");
  return 0;
}

template <typename T>
int fst_index(int argc,char **argv){
  if(argc<1){
    fprintf(stderr,"Must supply afile.saf.idx [chrname, write more info]\n");
    return 0; 
  }
  args *arg = getArgs(argc,argv);
  

  std::vector<persaf *> &saf =arg->saf;
  assert(saf.size()==2);
  int nSites = arg->nSites;
  if(nSites == 0){//if no -nSites is specified
    nSites=nsites(saf,arg);
  }
  if(fsizes<T>(saf,nSites)>getTotalSystemMemory())
    fprintf(stderr,"\t-> Looks like you will allocate too much memory, consider starting the program with a lower -nSites argument\n"); 
    
  fprintf(stderr,"\t-> nSites: %d\n",nSites);
  float bytes_req_megs = fsizes<T>(saf,nSites)/1024/1024;
  float mem_avail_megs = getTotalSystemMemory()/1024/1024;//in percentile
  //fprintf(stderr,"en:%zu to:%f\n",bytes_req_megs,mem_avail_megs);
  fprintf(stderr,"\t-> The choice of -nSites will require atleast: %f megabyte memory, that is atleast: %.2f%% of total memory\n",bytes_req_megs,bytes_req_megs*100/mem_avail_megs);

  std::vector<Matrix<T> *> gls;
  for(int i=0;i<saf.size();i++)
    gls.push_back(alloc<T>(nSites,saf[i]->nChr+1));

  int ndim= parspace(saf);
  double *sfs=new double[ndim];
  
  if(arg->sfsfname!=NULL)
      readSFS(arg->sfsfname,ndim,sfs);
  else
    for(int i=0;i<ndim;i++)
      sfs[i] = 1.0/((double)(ndim));
  normalize(sfs,ndim);
  
  double *a1,*b1;
  calcCoef(saf[0]->nChr,saf[1]->nChr,&a1,&b1);
#if 0
  for(int i=0;i<ndim;i++)
    fprintf(stdout,"%f %f\n",a1[i],b1[i]);
  exit(0);
#endif

  while(1) {
    int ret=readdata(saf,gls,nSites,arg->chooseChr,arg->start,arg->stop);//read nsites from data
    
    if(ret==-2&gls[0]->x==0)//no more data in files or in chr, eith way we break;
      break;
    
    if(saf.size()==1){
      if(ret!=-2){
	if(gls[0]->x!=nSites&&arg->chooseChr==NULL&&ret!=-3){
	  //	  fprintf(stderr,"continue continue\n");
	  continue;
	}
      }
    }else{
      if(gls[0]->x!=nSites&&arg->chooseChr==NULL&&ret!=-3){
	//fprintf(stderr,"continue continue\n");
	continue;
      }

    }
        
    fprintf(stderr,"\t-> Will now do fst temp dump using a chunk of %lu\n",gls[0]->x);
    block_coef(gls[0],gls[1],sfs,a1,b1);

    for(int i=0;i<gls.size();i++)
      gls[i]->x =0;
    
    if(ret==-2&&arg->chooseChr!=NULL)
      break;
    if(arg->onlyOnce)
      break;
  }

  destroy(gls,nSites);
  destroy_args(arg);
  delete [] sfs;
  
  fprintf(stderr,"\n\t-> NB NB output is no longer log probs of the frequency spectrum!\n");
  fprintf(stderr,"\t-> Output is now simply the expected values! \n");
  fprintf(stderr,"\t-> You can convert to the old format simply with log(norm(x))\n");
  return 0;
}

int main(int argc,char **argv){
#if 0
  char **reg = new char*[6];
  reg[0] = strdup("avsdf:");
  reg[1] = strdup("avsdf");
  reg[2] = strdup("avsdf:200");
  reg[3] = strdup("avsdf:200-300");
  reg[4] = strdup("avsdf:-300");
  reg[5] = strdup("avsdf:300-");

  int a,b;
  char *ref;
  for(int i=0;i<6;i++){
  fprintf(stderr,"%d) string=%s ",i,reg[i]);
  get_region(reg[i],&ref,a,b);
  fprintf(stderr," parsed as: ref:\'%s\' a:\'%d\' b:\'%d\'\n",ref,a,b);
}
  return 0;
#endif


  
  //start of signal handling
  struct sigaction sa;
  sigemptyset (&sa.sa_mask);
  sa.sa_flags = 0;
  sa.sa_handler = handler;
  sigaction(SIGPIPE, &sa, 0);
  sigaction(SIGINT, &sa, 0);  
\
  if(argc==1){
    //    fprintf(stderr, "\t->------------------\n\t-> ./realSFS\n\t->------------------\n");
    // fprintf(stderr,"\t-> This is the new realSFS program which works on the newer binary files from ANGSD!!\n");
    fprintf(stderr, "\t-> ---./realSFS------\n\t-> EXAMPLES FOR ESTIMATING THE (MULTI) SFS:\n\n\t-> Estimate the SFS for entire genome??\n");
    fprintf(stderr,"\t-> ./realSFS afile.saf.idx \n");
    fprintf(stderr, "\n\t-> 1) Estimate the SFS for entire chromosome 22 ??\n");
    fprintf(stderr,"\t-> ./realSFS afile.saf.idx -r chr22 \n");
    fprintf(stderr, "\n\t-> 2) Estimate the 2d-SFS for entire chromosome 22 ??\n");
    fprintf(stderr,"\t-> ./realSFS afile1.saf.idx  afile2.saf.idx -r chr22 \n");

    fprintf(stderr, "\n\t-> 3) Estimate the SFS for the first 500megabases (this will span multiple chromosomes) ??\n");
    fprintf(stderr,"\t-> ./realSFS afile.saf.idx -nSites 500000000 \n");

    fprintf(stderr, "\n\t-> 4) Estimate the SFS around a gene ??\n");
    fprintf(stderr,"\t-> ./realSFS afile.saf.idx -r chr2:135000000-140000000 \n");
    fprintf(stderr, "\n\t-> Other options [-P nthreads -tole tolerence_for_breaking_EM -maxIter max_nr_iterations ]\n");
    

    fprintf(stderr,"\n\t->------------------\n\t-> NB: Output is now counts of sites instead of log probs!!\n");
    fprintf(stderr,"\t-> NB: You can print data with ./realSFS print afile.saf.idx !!\n");
    fprintf(stderr,"\t-> NB: Higher order SFS's can be estimated by simply supplying multiple .saf.idx files!!\n");
    fprintf(stderr,"\t-> NB: Program uses accelerated EM, to use standard EM supply -m 0 \n");
    return 0;
  }
  ++argv;
  --argc;

  if(isatty(fileno(stdout))){
    fprintf(stderr,"\t-> You are printing the optimized SFS to the terminal consider dumping into a file\n");
    fprintf(stderr,"\t-> E.g.: \'./realSFS");
    for(int i=0;i<argc;i++)
      fprintf(stderr," %s",argv[i]);
    fprintf(stderr," >sfs.ml.txt\'\n");   

  }
  
  
  if(!strcasecmp(*argv,"printOld"))
    printOld(--argc,++argv);
  else if(!strcasecmp(*argv,"print"))
    print(--argc,++argv);
#if 0
  else if(!strcasecmp(*argv,"index"))
    index(--argc,++argv);
#endif
  else if(!strcasecmp(*argv,"fst_index"))
    fst_index<float>(--argc,++argv);
  else {
    args *arg = getArgs(argc,argv);
    if(!arg)
      return 0;
    main_opt<float>(arg);
    
  }

  return 0;
}
