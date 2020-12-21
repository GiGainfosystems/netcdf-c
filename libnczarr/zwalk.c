/*
Additonal optimizations:
1. slice covers all of exactly one chunk: we can just tranfer whole chunk to/from memory

*/
/*********************************************************************
 *   Copyright 2018, UCAR/Unidata
 *   See netcdf/COPYRIGHT file for copying and redistribution conditions.
 *********************************************************************/
#include "zincludes.h"

#define WDEBUG
#undef DFALTOPTIMIZE

static int initialized = 0;

static unsigned int optimize = 0;

extern int NCZ_buildchunkkey(size_t R, const size64_t* chunkindices, char** keyp);

/* 0 => no debug */
static unsigned int wdebug = 0;

/* Forward */
static int NCZ_walk(NCZProjection** projv, NCZOdometer* chunkodom, NCZOdometer* slpodom, NCZOdometer* memodom, const struct Common* common, void* chunkdata);
static int rangecount(NCZChunkRange range);
static int readfromcache(void* source, size64_t* chunkindices, void** chunkdata);
static int NCZ_fillchunk(void* chunkdata, struct Common* common);
static int transfern(const struct Common* common, unsigned char* slpptr0, unsigned char* memptr0,
                     size_t count, size_t slpstride, size_t memstride, void* data);    
static int iswholevar(struct Common* common,NCZSlice*);

const char*
astype(int typesize, void* ptr)
{
    switch(typesize) {
    case 4: {
	static char is[8];
	snprintf(is,sizeof(is),"%u",*((unsigned int*)ptr));
	return is;
        } break;
    default: break;
    }
    return "?";
}

/**************************************************/
int
ncz_chunking_init(void)
{
    const char* val = NULL;
#ifdef DFALTOPTIMIZE
    val = getenv("NCZ_NOOPTIMIZATION");
    optimize = (val == NULL ? 1 : 0);
#else
    optimize = 0;
#endif
    val = getenv("NCZ_WDEBUG");
    wdebug = (val == NULL ? 0 : atoi(val));
#ifdef WDEBUG
    if(wdebug > 0) fprintf(stderr,"wdebug=%u\n",wdebug);
#endif
    initialized = 1;
    return NC_NOERR;
}

/**************************************************/

/**
Goal: Given the slices being applied to the variable, create
and walk all possible combinations of projection vectors that
can be evaluated to provide the output data.
Note that we do not actually pass NCZSlice but rather
(start,count,stride) vectors.

@param var Controlling variable
@param usreading reading vs writing
@param start start vector
@param stop stop vector
@param stride stride vector
@param memory target or source of data
@param typecode nc_type of type being written
@param walkfcn fcn parameter to actually transfer data
*/

int
NCZ_transferslice(NC_VAR_INFO_T* var, int reading,
		  size64_t* start, size64_t* count, size64_t* stride,
		  void* memory, nc_type typecode)
{
    int r,stat = NC_NOERR;
    size64_t dimlens[NC_MAX_VAR_DIMS];
    size64_t chunklens[NC_MAX_VAR_DIMS];
    size64_t memshape[NC_MAX_VAR_DIMS];
    NCZSlice slices[NC_MAX_VAR_DIMS];
    struct Common common;
    NCZ_FILE_INFO_T* zfile = NULL;
    NCZ_VAR_INFO_T* zvar = NULL;
    size_t typesize;

    if(!initialized) ncz_chunking_init();

    if((stat = NC4_inq_atomic_type(typecode, NULL, &typesize))) goto done;

    if(wdebug >= 1) {
        size64_t stopvec[NC_MAX_VAR_DIMS];
	for(r=0;r<var->ndims;r++) stopvec[r] = start[r]+(count[r]*stride[r]);
        fprintf(stderr,"var: name=%s",var->hdr.name);
        fprintf(stderr," start=%s",nczprint_vector(var->ndims,start));
        fprintf(stderr," count=%s",nczprint_vector(var->ndims,count));
        fprintf(stderr," stop=%s",nczprint_vector(var->ndims,stopvec));
        fprintf(stderr," stride=%s\n",nczprint_vector(var->ndims,stride));
    }

    /* Fill in common */
    memset(&common,0,sizeof(common));
    common.var = var;
    common.file = (var->container)->nc4_info;
    zfile = common.file->format_file_info;
    zvar = common.var->format_var_info;

    common.reading = reading;
    common.memory = memory;
    common.typesize = typesize;
    common.cache = zvar->cache;

    if((stat = ncz_get_fill_value(common.file, common.var, &common.fillvalue))) goto done;

    /* We need to talk scalar into account */
    common.rank = var->ndims + zvar->scalar;
    common.scalar = zvar->scalar;
    common.swap = (zfile->native_endianness == var->endianness ? 0 : 1);

    common.chunkcount = 1;
    for(r=0;r<common.rank+common.scalar;r++) {
	if(common.scalar)
	    dimlens[r] = 1;
	else
	    dimlens[r] = var->dim[r]->len;
	chunklens[r] = var->chunksizes[r];
	slices[r].start = start[r];
	slices[r].stride = stride[r];
	slices[r].stop = minimum(start[r]+(count[r]*stride[r]),dimlens[r]);
	slices[r].len = dimlens[r];
	common.chunkcount *= chunklens[r];
	memshape[r] = count[r];
    }

    if(wdebug >= 1) {
        fprintf(stderr,"\trank=%d",common.rank);
        if(!common.scalar) {
            fprintf(stderr," dimlens=%s",nczprint_vector(common.rank,dimlens));
            fprintf(stderr," chunklens=%s",nczprint_vector(common.rank,chunklens));
            fprintf(stderr," memshape=%s",nczprint_vector(common.rank,memshape));
        }
	fprintf(stderr,"\n");
    }
    common.dimlens = dimlens; /* BAD: storing stack vector in a pointer; do not free */
    common.chunklens = chunklens; /* ditto */
    common.memshape = memshape; /* ditto */
    common.reader.source = ((NCZ_VAR_INFO_T*)(var->format_var_info))->cache;
    common.reader.read = readfromcache;

    if(common.scalar) {
        if((stat = NCZ_transferscalar(&common))) goto done;
    }
    else {
        if((stat = NCZ_transfer(&common, slices))) goto done;
    }
done:
    NCZ_clearcommon(&common);
    return stat;
}

/*
Walk the possible projections.
Broken out so we can use it for unit testing
@param common common parameters
@param slices
*/
int
NCZ_transfer(struct Common* common, NCZSlice* slices)
{
    int stat = NC_NOERR;
    NCZOdometer* chunkodom =  NULL;
    NCZOdometer* slpodom = NULL;
    NCZOdometer* memodom = NULL;
    void* chunkdata = NULL;
    int wholevar = 0;

    /*
     We will need three sets of odometers.
     1. Chunk odometer to walk the chunk ranges to get all possible
        combinations of chunkranges over all dimensions.
     2. For each chunk odometer set of indices, we need a projection
        odometer that walks the set of projection slices for a given
        set of chunk ranges over all dimensions.
     3. A memory odometer that walks the memory data to specify
        the locations in memory for read/write
    */     

    if(wdebug >= 2) {
	fprintf(stderr,"slices=%s\n",nczprint_slices(common->rank,slices));
    }

    if((stat = NCZ_projectslices(common->dimlens, common->chunklens, slices,
		  common, &chunkodom)))
	goto done;

    if(wdebug >= 4)
	fprintf(stderr,"allprojections:\n%s",nczprint_allsliceprojections(common->rank,common->allprojections)); fflush(stderr);

    wholevar = iswholevar(common,slices);

    if(wholevar) {
	size64_t* chunkindices = nczodom_indices(chunkodom);

        /* Implement a whole var read optimization; this is a rare occurrence
           where the variable has a single chunk and we are reading the whole chunk
        */

	if(wdebug >= 1)
	    fprintf(stderr,"case: wholevar:\n");

   	    /*  we are transfering the whole singular chunk */

	    /* Read the chunk */
	if(wdebug >= 1) {
	    fprintf(stderr,"chunkindices: %s\n",nczprint_vector(common->rank,chunkindices));
	}

            switch ((stat = common->reader.read(common->reader.source, chunkindices, &chunkdata))) {
            case NC_ENOTFOUND: /* cache created the chunk */
	        if((stat = NCZ_fillchunk(chunkdata,common))) goto done;
	        break;
            case NC_NOERR: break;
            default: goto done;
            }

	    /* Figure out memory address */
	    unsigned char* memptr = ((unsigned char*)common->memory);
	    unsigned char* slpptr = ((unsigned char*)chunkdata);

	    transfern(common,slpptr,memptr,common->chunkcount,1,1,chunkdata);
	    if(zutest && zutest->tests & UTEST_WHOLEVAR)
	        zutest->print(UTEST_WHOLEVAR, common);


	goto done;
    }

    /* iterate over the odometer: all combination of chunk
       indices in the projections */
    for(;nczodom_more(chunkodom);) {
	int r;
	size64_t* chunkindices = NULL;
        NCZSlice slpslices[NC_MAX_VAR_DIMS];
        NCZSlice memslices[NC_MAX_VAR_DIMS];
        NCZProjection* proj[NC_MAX_VAR_DIMS];
	size64_t shape[NC_MAX_VAR_DIMS];

	chunkindices = nczodom_indices(chunkodom);
	if(wdebug >= 1) {
	    fprintf(stderr,"chunkindices: %s\n",nczprint_vector(common->rank,chunkindices));
	}

	for(r=0;r<common->rank;r++) {
	    NCZSliceProjections* slp = &common->allprojections[r];
	    NCZProjection* projlist = slp->projections;
	    size64_t indexr = chunkindices[r];
  	    /* use chunkindices[r] to find the corresponding projection slice */
	    /* We must take into account that the chunkindex of projlist[r]
               may be greater than zero */
	    /* note the 2 level indexing */
	    indexr -= slp->range.start;
	    NCZProjection* pr = &projlist[indexr];
	    proj[r] = pr;
	}

	if(wdebug > 0) {
  	    fprintf(stderr,"Selected projections:\n");
	    for(r=0;r<common->rank;r++) {
  	        fprintf(stderr,"\t[%d] %s\n",r,nczprint_projection(*proj[r]));
		shape[r] = proj[r]->iocount;
	    }
	    fprintf(stderr,"\tshape=%s\n",nczprint_vector(common->rank,shape));
	}

	for(r=0;r<common->rank;r++) {
	    slpslices[r] = proj[r]->chunkslice;
	    memslices[r] = proj[r]->memslice;
	}
	if(zutest && zutest->tests & UTEST_TRANSFER)
	    zutest->print(UTEST_TRANSFER, common, chunkodom, slpslices, memslices);

        /* Read from cache */
        stat = common->reader.read(common->reader.source, chunkindices, &chunkdata);
	switch (stat) {
        case NC_ENOTFOUND: /* cache created the chunk */
	    if((stat = NCZ_fillchunk(chunkdata,common))) goto done;
	    break;
        case NC_NOERR: break;
        default: goto done;
        }

	slpodom = nczodom_fromslices(common->rank,slpslices);
	memodom = nczodom_fromslices(common->rank,memslices);

	{ /* walk with odometer, possibly optimized */
	    if(wdebug >= 1)
	    fprintf(stderr,"case: odometer; slp.optimized=%d:\n",slpodom->properties.optimized);
  	    /* This is the key action: walk this set of slices and transfer data */
  	    if((stat = NCZ_walk(proj,chunkodom,slpodom,memodom,common,chunkdata))) goto done;
	}
        nczodom_free(slpodom); slpodom = NULL;
        nczodom_free(memodom); memodom = NULL;
        nczodom_next(chunkodom);
    }
done:
    nczodom_free(slpodom);
    nczodom_free(memodom);
    nczodom_free(chunkodom);
    return stat;
}


/*
@param projv
@param chunkodom
@param slpodom
@param memodom
@param common
@param chunkdata
@return NC_NOERR
*/
static int
NCZ_walk(NCZProjection** projv, NCZOdometer* chunkodom, NCZOdometer* slpodom, NCZOdometer* memodom, const struct Common* common, void* chunkdata)
{
    int stat = NC_NOERR;

    for(;;) {
        if(nczodom_more(slpodom)) {
            size64_t slpoffset = 0;
            size64_t memoffset = 0;
            unsigned char* memptr0 = NULL;
            unsigned char* slpptr0 = NULL;

            if(wdebug >= 3) {
		fprintf(stderr,"xx.slp: odom: %s\n",nczprint_odom(slpodom));
		fprintf(stderr,"xx.mem: odom: %s\n",nczprint_odom(memodom));
	    }

            /* Convert the indices to a linear offset WRT to chunk indices */
            slpoffset = nczodom_offset(slpodom);
            memoffset = nczodom_offset(memodom);

            /* transfer data */
            memptr0 = ((unsigned char*)common->memory)+(memoffset * common->typesize);
            slpptr0 = ((unsigned char*)chunkdata)+(slpoffset * common->typesize);

	    LOG((1,"%s: slpptr0=%p memptr0=%p slpoffset=%llu memoffset=%lld",__func__,slpptr0,memptr0,slpoffset,memoffset));
	    if(zutest && zutest->tests & UTEST_WALK)
		zutest->print(UTEST_WALK, common, chunkodom, slpodom, memodom);
	    if((stat = transfern(common,slpptr0,memptr0,nczodom_avail(slpodom),
	                         nczodom_laststride(slpodom),nczodom_lastlen(memodom),
				 chunkdata))) goto done;
            nczodom_next(memodom);
        } else break; /* slpodom exhausted */
        nczodom_next(slpodom);
    }
done:
    return stat;    
}

#ifdef WDEBUG
static void
wdebug1(const struct Common* common, unsigned char* srcptr, unsigned char* dstptr, size_t count, size_t srcstride, size_t dststride, void* chunkdata, const char* tag)
{
    unsigned char* dstbase = (common->reading?common->memory:chunkdata);
    unsigned char* srcbase = (common->reading?chunkdata:common->memory);
    unsigned dstoff = (unsigned)(dstptr - dstbase);
    unsigned srcoff = (unsigned)(srcptr - srcbase);
    unsigned srcidx = srcoff / sizeof(unsigned);

    fprintf(stderr,"%s: %s: [%u] %u/%d->%u/%u",
	    tag,
	    common->reading?"read":"write",
	    (unsigned)count,
	    (unsigned)(srcoff/common->typesize),
    	    (unsigned)srcstride,
	    (unsigned)(dstoff/common->typesize),
       	    (unsigned)dststride
	    );
    fprintf(stderr,"\t%s[%u]=%u\n",(common->reading?"chunkdata":"memdata"),
//      0,((unsigned*)srcptr)[0]
        srcidx,((unsigned*)srcbase)[srcidx]
	);
#if 0
    { size_t len = common->typesize*count;
    fprintf(stderr," | [%u] %u->%u\n",(unsigned)len,(unsigned)srcoff,(unsigned)dstoff);
    }
#endif
    fprintf(stderr,"\n");
}
#else
#define wdebug1(common,srcptr,dstptr,count,srcstride,dststride,chunkdata,tag)
#endif

static int
transfern(const struct Common* common, unsigned char* slpptr, unsigned char* memptr, size_t count, size_t slpstride, size_t memstride, void* chunkdata)
{
    int stat = NC_NOERR;
    size_t typesize = common->typesize;
    size_t len = typesize*count;
    size_t m,s;

    if(common->reading) {
	if(wdebug >= 2)
            wdebug1(common, slpptr, memptr, count, slpstride, memstride, chunkdata, "transfern");

	if(slpstride == 1 && memstride == 1)
            memcpy(memptr,slpptr,len); /* straight copy */
	else {
	    for(m=0,s=0;s<count;s+=slpstride,m+=memstride) {
		size_t soffset = s*typesize;
		size_t moffset = m*typesize;
	        memcpy(memptr+moffset,slpptr+soffset,typesize);
	    }
	}
        if(common->swap)
            NCZ_swapatomicdata(len,memptr,common->typesize);
    } else { /*writing*/
if(wdebug >= 2)
        wdebug1(common, memptr, slpptr, count, memstride, slpstride, chunkdata,"transfern");

unsigned char* srcbase = (common->reading?chunkdata:common->memory);
unsigned srcoff = (unsigned)(memptr - srcbase);
unsigned srcidx = srcoff / sizeof(unsigned); (void)srcidx;
	if(slpstride == 1)
            memcpy(slpptr,memptr,len); /* straight copy */
	else {
	    for(m=0,s=0;s<count;s+=slpstride,m+=memstride) {
		size_t soffset = s*typesize;
		size_t moffset = m*typesize;
	        memcpy(slpptr+soffset,memptr+moffset,typesize);
	        if(wdebug >= 3 && m > 0)
	            wdebug1(common, memptr+moffset, slpptr+soffset, 1, slpstride, memstride, chunkdata, "\t");

	    }
	}
        if(common->swap)
            NCZ_swapatomicdata(len,slpptr,common->typesize);
    }
    return THROW(stat);
}

/* This function may not be necessary if code in zvar does it instead */
static int
NCZ_fillchunk(void* chunkdata, struct Common* common)
{
    int stat = NC_NOERR;    

    if(common->fillvalue == NULL) {
        memset(chunkdata,0,common->chunkcount*common->typesize);
	goto done;
    }	

    if(common->cache->fillchunk == NULL) {
        /* Get fill chunk*/
        if((stat = NCZ_create_fill_chunk(common->cache->chunksize, common->typesize, common->fillvalue, &common->cache->fillchunk)))
	    goto done;
    }
    memcpy(chunkdata,common->cache->fillchunk,common->cache->chunksize);
done:
    return stat;
}

/* Break out this piece so we can use it for unit testing */
int
NCZ_projectslices(size64_t* dimlens,
                  size64_t* chunklens,
                  NCZSlice* slices,
                  struct Common* common, 
                  NCZOdometer** odomp)
{
    int stat = NC_NOERR;
    int r;
    NCZOdometer* odom = NULL;
    NCZSliceProjections* allprojections = NULL;
    NCZChunkRange ranges[NC_MAX_VAR_DIMS];
    size64_t start[NC_MAX_VAR_DIMS];
    size64_t stop[NC_MAX_VAR_DIMS];
    size64_t stride[NC_MAX_VAR_DIMS];
    size64_t len[NC_MAX_VAR_DIMS];

    if((allprojections = calloc(common->rank,sizeof(NCZSliceProjections))) == NULL)
        {stat = NC_ENOMEM; goto done;}
    memset(ranges,0,sizeof(ranges));

    /* Package common arguments */
    common->dimlens = dimlens;
    common->chunklens = chunklens;
    /* Compute the chunk ranges for each slice in a given dim */
    if((stat = NCZ_compute_chunk_ranges(common->rank,slices,common->chunklens,ranges)))
        goto done;

    /* Compute the slice index vector */
    if((stat=NCZ_compute_all_slice_projections(common,slices,ranges,allprojections)))
        goto done;

    /* Verify */
    for(r=0;r<common->rank;r++) {
        assert(rangecount(ranges[r]) == allprojections[r].count);
    }

    /* Compute the shape vector */
    for(r=0;r<common->rank;r++) {
        int j;
        size64_t iocount = 0;
        NCZProjection* projections = allprojections[r].projections;
        for(j=0;j<allprojections[r].count;j++) {
            NCZProjection* proj = &projections[j];
            iocount += proj->iocount;
        }
        common->shape[r] = iocount;
    }
    common->allprojections = allprojections;
    allprojections = NULL;

    /* Create an odometer to walk all the range combinations */
    for(r=0;r<common->rank;r++) {
        start[r] = ranges[r].start; 
        stop[r] = ranges[r].stop;
        stride[r] = 1;
        len[r] = ceildiv(common->dimlens[r],common->chunklens[r]);
    }   

    if((odom = nczodom_new(common->rank,start,stop,stride,len)) == NULL)
        {stat = NC_ENOMEM; goto done;}
    if(odomp) *odomp = odom;

done:
    return stat;
}

/***************************************************/
/* Utilities */

static int
rangecount(NCZChunkRange range)
{
    return (range.stop - range.start);
}

/* Goal: Given a set of per-dimension indices,
     compute the corresponding linear position.
*/
size64_t
NCZ_computelinearoffset(size_t R, const size64_t* indices, const size64_t* dimlens)
{
      size64_t offset;
      int i;

      offset = 0;
      for(i=0;i<R;i++) {
          offset *= dimlens[i];
          offset += indices[i];
      } 
      return offset;
}

#if 0
/* Goal: Given a linear position
     compute the corresponding set of R indices
*/
void
NCZ_offset2indices(size_t R, size64_t offset, const size64_t* dimlens, size64_t* indices)
{
      int i;

      for(i=0;i<R;i++) {
          indices[i] = offset % dimlens[i];
          offset = offset / dimlens[i];
      } 
}
#endif

/**************************************************/
/* Unit test entry points */

int
NCZ_chunkindexodom(int rank, const NCZChunkRange* ranges, size64_t* chunkcounts, NCZOdometer** odomp)
{
    int stat = NC_NOERR;
    int r;
    NCZOdometer* odom = NULL;
    size64_t start[NC_MAX_VAR_DIMS];
    size64_t stop[NC_MAX_VAR_DIMS];
    size64_t stride[NC_MAX_VAR_DIMS];
    size64_t len[NC_MAX_VAR_DIMS];

    for(r=0;r<rank;r++) {
        start[r] = ranges[r].start; 
        stop[r] = ranges[r].stop;
        stride[r] = 1;
        len[r] = chunkcounts[r];
    }   

    if((odom = nczodom_new(rank, start, stop, stride, len))==NULL)
        {stat = NC_ENOMEM; goto done;}

    if(odomp) {*odomp = odom; odom = NULL;}

done:
    nczodom_free(odom);
    return stat;
}

static int
readfromcache(void* source, size64_t* chunkindices, void** chunkdatap)
{
    return NCZ_read_cache_chunk((struct NCZChunkCache*)source, chunkindices, chunkdatap);
}

void
NCZ_clearcommon(struct Common* common)
{
    NCZ_clearsliceprojections(common->rank,common->allprojections);
    nullfree(common->allprojections);
    nullfree(common->fillvalue);
}

/* Does the User want the whole variable? */
static int
iswholevar(struct Common* common, NCZSlice* slices)
{
    int i;
    
    /* Check that slices cover whole file */
    for(i=0;i<common->rank;i++) {
	if(slices[i].start != 0
	   || slices[i].stop != common->dimlens[i]
   	   || slices[i].stride != 1)
	    return 0; /* slices do not cover the whole file */
    }
    /* Check that there is only one chunk */
    for(i=0;i<common->rank;i++) {
	if(common->dimlens[i] != common->chunklens[i])
	    return 0; /* must be more than one chunk */
    }
    return 1;
}

/**************************************************/
/* Scalar variable support */

/*
@param common common parameters
*/

int
NCZ_transferscalar(struct Common* common)
{
    int stat = NC_NOERR;
    void* chunkdata = NULL;
    size64_t chunkindices[NC_MAX_VAR_DIMS];
    unsigned char* memptr, *slpptr;

    /* Read from single chunk from cache */
    chunkindices[0] = 0;
    switch ((stat = common->reader.read(common->reader.source, chunkindices, &chunkdata))) {
    case NC_ENOTFOUND: /* cache created the chunk */
        if((stat = NCZ_fillchunk(chunkdata,common))) goto done;
	break;
    case NC_NOERR: break;
    default: goto done;
    }

    /* Figure out memory address */
    memptr = ((unsigned char*)common->memory);
    slpptr = ((unsigned char*)chunkdata);
    if(common->reading)
	memcpy(memptr,slpptr,common->chunkcount*common->typesize);
    else
	memcpy(slpptr,memptr,common->chunkcount*common->typesize);

done:
    return stat;
}

/* Debugging Interface: return the contents of a specified chunk */
EXTERNL int
NCZ_read_chunk(int ncid, int varid, size64_t* zindices, void* chunkdata)
{
    int stat = NC_NOERR;
    NC_VAR_INFO_T* var = NULL;
    NCZ_VAR_INFO_T* zvar;
    struct NCZChunkCache* cache = NULL;
    void* cachedata = NULL;

    if ((stat = nc4_find_grp_h5_var(ncid, varid, NULL, NULL, &var)))
	return THROW(stat);
    zvar = (NCZ_VAR_INFO_T*)var->format_var_info;
    cache = zvar->cache;

    if((stat = NCZ_read_cache_chunk(cache,zindices,&cachedata))) goto done;
    if(chunkdata)
        memcpy(chunkdata,cachedata,cache->chunksize);
    
done:
    return stat;
}
