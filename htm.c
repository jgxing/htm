#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/freeglut.h>


#define DENDRITE_CACHE 0x1000
#define SYNAPSES 32

#define DECAY 4
#define NOISE_FACTOR 5.0
#define IS_ACTIVE 0x80
#define WAS_ACTIVE (IS_ACTIVE>>DECAY)
#define SYNAPSE_ADJUSTMENT 1

#define PTHRESH 0x80


#define MIN(x,y) ((x)<(y)?(x):(y))
#define MAX(x,y) ((x)>(y)?(x):(y))
#define ABS(x) ((x)<0?(-x):(x))
#define BZERO(x) bzero((&x),sizeof(x))

typedef struct { unsigned short s[0]; void *d1; void *d2; } Seed;
#define RESEED(seed,ptr) (seed.d1=seed.d2=ptr,seed.s)
#define NRAND(seed) (nrand48(seed.s)) // 0 through 2^31
#define JRAND(seed) (jrand48(seed.s)) // -2^31 through 2^31
#define ERAND(seed) (erand48(seed.s)) // 0.0 through 1.0
#define DRAND(seed) (ERAND(seed)*2.0-1.0) // -1.0 through 1.0


typedef struct { int v[0]; int x,y,z,vol; } D3;
#define VOL3D(i) ((i)[3]=(i)[0]*(i)[1]*(i)[2])

#define DIM3(x,y,z,d) ((x)*(d)[1]*(d)[2] + (y)*(d)[2] + (z))
#define DIM3D(i,d) (((i)[3]=DIM3((i)[0],(i)[1],(i)[2],(d))),((i)[3]<0?-1:((i)[3]>(d)[3]?-1:(i)[3])))
#define CLIP3D(i,d) (DIM3D((i),(d)),(i)[0]>=0 && (i)[0]<(d)[0] && (i)[1]>=0 && (i)[1]<(d)[1] && (i)[2]>=0 && (i)[2]<(d)[2])

#define WRAP3(x,y,z,d) (((x)%=(d)[0])*(d)[1]*(d)[2] + ((y)%=(d)[1])*(d)[2] + ((z%=(d)[2])))
#define WRAP3D(i,d) ((i)[3]=WRAP3((i)[0],(i)[1],(i)[2],(d)))

#define LOOP(i,from,to) for ((i)=(from);(i)<(to);(i)++)
#define ZLOOP(i,lim) LOOP((i),0,lim)
#define LOOPD3(iv,fromv,tov) LOOP((iv)[0],(fromv)[0],(tov)[0]) LOOP((iv)[1],(fromv)[1],(tov)[1]) LOOP((iv)[2],(fromv)[2],(tov)[2])
#define ZLOOPD3(iv,limv) LOOP((iv)[0],0,(limv)[0]) LOOP((iv)[1],0,(limv)[1]) LOOP((iv)[2],0,(limv)[2])


typedef struct { int   v[0]; int   x,y,z; } ivec;
typedef struct { char  v[0]; char  x,y,z; } cvec;
typedef struct { float v[0]; float x,y,z; } fvec;

typedef struct { cvec offset[SYNAPSES]; } DendriteMap;

DendriteMap gDendriteMap[DENDRITE_CACHE];
#define DENDRITEMAP(i) (gDendriteMap[(i)%DENDRITE_CACHE])

typedef enum { SPATIAL,TEMPORAL,GENERATIVE } HTM_STAGE;

int cycles=0;
Seed gseed={{},NULL,NULL};

int show_cells=1;
int show_dendrites=1;
int show_map=0;
int show_scores=0;
int show_suppression=0;
int show_risers=0;
int show_predictions=1;
int show_tex=0;
int hide_input=0;
int do_generative=1;


/********************************************************************************************
 *
 * Structures
 *
 */
typedef struct
{
    unsigned char permanence;
} Synapse;

typedef struct
{
    Synapse *synapse;
    unsigned char sensitivity;
    short score;
} Dendrite;

typedef struct
{
    Dendrite *dendrite;
    char bias;
} Dendrites;
    
typedef struct
{
    D3 size;
    fvec position;
    unsigned char *active;  // 50% decay per tick
    unsigned char *predicted;
    float *score;
    float *suppression;
} StateMap;

typedef struct
{
    StateMap *input;
    StateMap *output;
    int breadth,depth; // dendrites,synapses
    D3 size,offset; // region of input to sample
    Dendrites *dendrites;
} Interface;

enum { FEEDFWD,INTRA,FEEDBACK,INTERFACES }; // inter-region interfaces

typedef struct
{
    StateMap states;
    Interface interface[INTERFACES];
    int dendrites;
} Region;

typedef struct 
{
    D3 size;
    fvec position;
    cvec breadth; // per interface
    cvec depth; // per interface
    int lowerlayer; // relative offset from this layer to it's lower-layer
    //D3 size,offset; // region of lower layer to sample
} RegionDesc;

typedef struct
{
    int regions;
    Region *region;
} Htm;



/********************************************************************************************
 *
 * Initialization
 *
 */
void DendriteMap_init()
{
    int i;
    
    void generate()
    {
        int synapse;
        unsigned long long r=NRAND(gseed);
        int v[]={1,(r&0x10000000)?1:-1};
        int xx=(r&0x2000000)?1:-1; // flip l/r
        int xy=(r&0x4000000)?1:0; // switch axes
        int mx[]={6,2};

        r<<=31;
        r|=NRAND(gseed);
        BZERO(DENDRITEMAP(i));
        
        LOOP(synapse,1,SYNAPSES)
        {
            if ((r&mx[v[0]]) && !(v[0]=!v[0]) && (r&1)) v[1]=-v[1];
            DENDRITEMAP(i).offset[synapse].v[xy] =v[0]*xx;
            DENDRITEMAP(i).offset[synapse].v[!xy]=v[0]?0:v[1];
            DENDRITEMAP(i).offset[synapse].v[2]  =(r&0xff000)>>12;
            r>>=1;
        }
    }
    
    ZLOOP(i,DENDRITE_CACHE) generate();
}


int StateMap_init(StateMap *map,D3 *size,fvec *position)
{
    int i;

    if (!map) return !0;
    VOL3D(size->v); // assigns to vol
    map->size=*size;
    map->position=*position;
    map->active=malloc(size->vol*sizeof(map->active[0]));
    map->predicted=malloc(size->vol*sizeof(map->predicted[0]));
    map->score=malloc(size->vol*sizeof(map->score[0]));
    map->suppression=malloc(size->vol*sizeof(map->suppression[0]));
    bzero(map->active,size->vol*sizeof(map->active[0]));
    bzero(map->predicted,size->vol*sizeof(map->predicted[0]));
    bzero(map->score,size->vol*sizeof(map->score[0]));
    bzero(map->suppression,size->vol*sizeof(map->suppression[0]));
    return 0;
}


int Interface_init(Interface *interface,StateMap *input,StateMap *output,int breadth,int depth)
{
    int i,d,s,r;
    unsigned char r2;

    int dendrites;

    if (!interface || !output || !input) return !0;
    interface->input=input;
    interface->output=output;
    interface->breadth=breadth;
    interface->depth=depth;
    
    interface->dendrites=malloc(output->size.vol * sizeof(Dendrites));
    ZLOOP(i,output->size.vol)
    {
        interface->dendrites[i].bias=0;
        interface->dendrites[i].dendrite=malloc(breadth*sizeof(Dendrite));
        ZLOOP(d,breadth)
        {
            r=NRAND(gseed);
            r2=r>>12;
            interface->dendrites[i].dendrite[d].sensitivity=r2;
            interface->dendrites[i].dendrite[d].synapse=malloc(depth*sizeof(Synapse));
            ZLOOP(s,depth)
                interface->dendrites[i].dendrite[d].synapse[s].permanence=PTHRESH;
        }
    }
    return 0;
}


int Region_init(Region *region,D3 *size,fvec *position)
{
    D3 s;
    if (!region || !size) return !0;
    BZERO(*region);
    StateMap_init(&region->states,size,position);
    return 0;
}


int Htm_init(Htm *htm,RegionDesc *rd,int regions)
{
    int r,ll,i;
    if (!htm || !regions || !rd) return !0;

    DendriteMap_init();

    htm->regions=regions;
    htm->region=malloc(sizeof(Region)*regions);
    ZLOOP(r,regions) Region_init(&htm->region[r],&rd[r].size,&rd[r].position);

    ZLOOP(r,regions)
    {
        ZLOOP(i,INTERFACES) htm->region[r].dendrites+=rd[r].breadth.v[i];
        
        Interface_init(&htm->region[r].interface[INTRA],
                       &htm->region[r].states,
                       &htm->region[r].states,
                       rd[r].breadth.v[INTRA],
                       rd[r].depth.v[INTRA]);

        if ((ll=rd[r].lowerlayer))
        {
            Interface_init(&htm->region[r].interface[FEEDFWD],
                           &htm->region[r-ll].states,
                           &htm->region[r].states,
                           rd[r].breadth.v[FEEDFWD],
                           rd[r].depth.v[FEEDFWD]);
            
            Interface_init(&htm->region[r-ll].interface[FEEDBACK],
                           &htm->region[r].states,
                           &htm->region[r-ll].states,
                           rd[r].breadth.v[FEEDBACK],
                           rd[r].depth.v[FEEDBACK]);
        }
    }
}



/********************************************************************************************
 *
 * Processing
 *
 */
typedef int (*Synapse_op)(D3 *ipos,D3 *opos,int dendrite,int synapse);

int Interface_traverse(Interface *interface,Synapse_op op)
{
    int status=0;
    Seed seed;
    D3 ipos,opos;
    fvec delta;
    ivec fanout;
    int d,s,axis;
    DendriteMap map;
    Synapse *syn;
    int i;
    int random;

    if (!interface || !interface->input || !interface->output) return !0;
    
    delta.x=(float) interface->input->size.x/(float) interface->output->size.x;
    delta.y=(float) interface->input->size.y/(float) interface->output->size.y;
    fanout.x=MAX((int) delta.x,1);
    fanout.y=MAX((int) delta.y,1);
    
    RESEED(seed,interface); // reseed when traversing an interface's dendrites
    
    ZLOOPD3(opos.v,interface->output->size.v)
    {
        (void) DIM3D(opos.v,interface->output->size.v); // populate opos.vol
        ZLOOP(d,interface->breadth)
        {
            random=NRAND(seed); random>>=4;
            ipos.x=(int) (opos.x*delta.x)+(random%fanout.x); random>>=4;
            ipos.y=(int) (opos.y*delta.y)+(random%fanout.y); random>>=4;
            map=DENDRITEMAP(random);
            ZLOOP(s,interface->depth)
            {
                ipos.x+=map.offset[s].x;
                ipos.y+=map.offset[s].y;
                ipos.z=map.offset[s].z%interface->input->size.z;
                if ((status=op(&ipos,&opos,d,s)))
                    goto done;
            }
        }
    }
 done:
    return status;
}

#define SHOW(fmt,args...) printf("%d,%d,%d %d,%d,%d %d %d " fmt "\n",                                   \
                                 ipos->x,ipos->y,ipos->z,opos->x,opos->y,opos->z,dendrite,synapse,      \
                                 args)

int Interface_score(Interface *interface)
{
    int dthresh=2;
    
    int synapse_op(D3 *ipos,D3 *opos,int dendrite,int synapse)
    {
        Dendrites *dens=&interface->dendrites[opos->vol];
        Dendrite *den=&dens->dendrite[dendrite];
        Synapse *syn=&den->synapse[synapse];
        double sp;
        
        double stoch_perm(unsigned char p) { return DRAND(gseed)*NOISE_FACTOR; }
    
        if (synapse==0)
        {
            den->score=0;
            if (interface->input==interface->output) return 0; // don't let cell use itself as input
        }
        
        sp=stoch_perm(syn->permanence);
                      
        if (CLIP3D(ipos->v,interface->input->size.v))
            if (syn->permanence+sp > PTHRESH)
                if (interface->input->active[ipos->vol] > den->sensitivity+dens->bias)
                {
                    den->score+=1;
                    if (interface->output->predicted[opos->vol] & IS_ACTIVE)
                        den->score+=1; // synapses that would be active and are predicted to be active get an extra boost
                }
        
        if (synapse==interface->depth-1)
            if (den->score >= dthresh)
                interface->output->score[opos->vol] += den->score;
        
        return 0;
    }

    return Interface_traverse(interface,synapse_op);
}


int Interface_select(Interface *interface)
{
    float synapses=interface->depth;
    int i;
    
#define SUPPRESSION ((synapses-synapse)/synapses)

    int synapse_op(D3 *ipos,D3 *opos,int dendrite,int synapse)
    {
        if (synapse>0 && CLIP3D(ipos->v,interface->input->size.v))
            interface->input->suppression[ipos->vol] += interface->output->score[opos->vol] * SUPPRESSION;
        return 0;
    }
    
    if (!interface->input || !interface->output || interface->output->size.vol==0)
        return 0;
    
    Interface_traverse(interface,synapse_op);
    
    ZLOOP(i,interface->output->size.vol)
        if (((interface->output->score[i])*synapses-interface->output->suppression[i]) > 0)
            interface->output->active[i]|=IS_ACTIVE;
    
    return 0;
}


int Interface_adjust(Interface *interface,HTM_STAGE stage)
{
#define INC(x,y) ((x)=MAX((x),(typeof(x))((x)+(y))))
#define DEC(x,y) ((x)=MIN((x),(typeof(x))((x)-(y))))
    
    int synapse_op(D3 *ipos,D3 *opos,int dendrite,int synapse)
    {
        Dendrites *dens=&interface->dendrites[opos->vol];
        Dendrite *den=&dens->dendrite[dendrite];
        Synapse *syn=&den->synapse[synapse];
        int adj=SYNAPSE_ADJUSTMENT;
        
        if (interface->input==interface->output && synapse==0) return 0; // don't let cell use itself as input
    
        if (CLIP3D(ipos->v,interface->input->size.v))
        {
            int in_state=0,out_state=0;

            if (stage==SPATIAL)
            {
                if (interface->input->active[ipos->vol] > den->sensitivity+dens->bias)
                {
                    in_state+=1;
                    if (interface->output->predicted[opos->vol] > den->sensitivity+dens->bias)
                        in_state+=2; // synapses that would be active and are predicted to be active get an extra boost
                }
            }
            else if (stage==TEMPORAL)
            {
                if (interface->input->active[ipos->vol])
                    in_state+=1;
            }
            
            out_state=interface->output->active[opos->vol] & IS_ACTIVE;
            
            if (in_state && out_state)
                INC(syn->permanence,adj*in_state); // increment if both "on"
            else if (!in_state && out_state)
                DEC(syn->permanence,adj); // decrement if only one or other is "on"
        }
        
        return 0;
    }
    
    return Interface_traverse(interface,synapse_op);
}


int Region_update(Region *region)
{
    int i,j;
    int interface;
    D3 opos;
    int dendrite;
    
    int spatial()
    {
        if (region->interface[FEEDFWD].input) // spatial/temporal pooler
        {
            ZLOOP(i,region->states.size.vol)
            {
                region->states.active[i]>>=DECAY;
                region->states.score[i]=0;
                region->states.suppression[i]=0;
            }

            Interface_score(&region->interface[FEEDFWD]);  // propagate inputs
            Interface_select(&region->interface[INTRA]);   // calculate post-supporessed activations
            Interface_adjust(&region->interface[FEEDFWD],SPATIAL); // update synapses
        }
        else
        {
            D3 p,a={{},1,1,0,0},b={{},6,6,1,0},c={{},7,7,1,0};
            static int offset=0;
        
            //printf("  Reading %d bytes:\n",region->states.size.vol);
            //ZLOOP(i,region->states.size.vol) region->states.active[i]=getchar();
            ZLOOP(i,region->states.size.vol) region->states.active[i]=0x00;
            if (!hide_input)
            {
                ZLOOPD3(p.v,c.v) region->states.active[(DIM3D(p.v,region->states.size.v)+offset)%region->states.size.vol]=0xff;
                LOOPD3(p.v,a.v,b.v) region->states.active[(DIM3D(p.v,region->states.size.v)+offset)%region->states.size.vol]=0x00;
            }
            offset++;
            //printf("  Done!\n");
        }
    }

    int temporal()
    {
        Interface_adjust(&region->interface[INTRA],TEMPORAL); // update synapses
        Interface_adjust(&region->interface[FEEDBACK],TEMPORAL); // update synapses
    }
    
    int generative()
    {
        ZLOOP(i,region->states.size.vol)
        {
            region->states.predicted[i]=0;
            region->states.score[i]=0;
        }
        
        Interface_score(&region->interface[INTRA]); // calculate post-suppressed predictions
        //Interface_score(&region->interface[FEEDBACK]); // calculate post-suppressed predictions
        ZLOOP(i,region->states.size.vol) if (region->states.score[i]) region->states.predicted[i]|=IS_ACTIVE;

    }
    
    if (!region) return !0;
    
    spatial();
    temporal();
    generative();
    
    return 0;
}


int Htm_update(Htm *htm)
{
    int r;
    if (!htm) return !0;
    ZLOOP(r,htm->regions) Region_update(&htm->region[r]);
    cycles++;

}


/********************************************************************************************
 *
 * OpenGL
 *
 */

int main(int argc, char **argv)
{
    int gwidth=400,gheight=400;
    
    float camera[] = { 6,-25,15 };
    float center[] = { 0,0,8 };

    float viewup[] = { 0,0,1 };
    float zoom=.35;

    int mousestate[6]={0,0,0,0,0,0};
    int mousepos[2]={0,0};

    
    Htm htm;
    RegionDesc rd[]= {
        //   size             pos         breadth        depth  ll
        {{{},16,16,1,0}, {{}, -8, -8, -8}, {{}, 0, 4, 8}, {{}, 0, 4, 8}, 0},
        {{{},32,32,1,0}, {{},-16,-16,  8}, {{},16, 4, 0}, {{}, 2, 8, 0}, 1},
        //{{{},16,16,4,0}, {{}, -8, -8, 8}, {{},16, 8, 8}, {{}, 2, 8, 8}, 1},
        //{{{}, 8, 8,4,0}, {{}, -4, -4,16}, {{}, 8, 8, 0}, {{}, 8, 8, 0}, 1}
    };
        
    Htm_init(&htm,rd,2);

    
    void display()
    {
        int r,i;
        
        int Region_display(Region *region)
        {
            Seed seed;
            D3 opos;
            fvec vertex;
            int axis;
            int state;
            float score,suppression;

            void draw_cell(float scale)
            {
                glVertex3f(vertex.v[0]-scale,vertex.v[1]-scale,vertex.v[2]);
                glVertex3f(vertex.v[0]-scale,vertex.v[1]+scale,vertex.v[2]);
                glVertex3f(vertex.v[0]+scale,vertex.v[1]+scale,vertex.v[2]);
                glVertex3f(vertex.v[0]+scale,vertex.v[1]-scale,vertex.v[2]);
            }
    
            if (!region) return !0;
    
            glBegin(GL_QUADS);
            ZLOOPD3(opos.v,region->states.size.v)
            {
                (void) DIM3D(opos.v,region->states.size.v);
                ZLOOP(axis,3) vertex.v[axis]=opos.v[axis]+region->states.position.v[axis];
                if (show_cells)
                {
                    int active=region->states.active[opos.vol];
                    int predict=region->states.predicted[opos.vol];
                    if (active||predict)
                    {
                        glColor4f(active/255.0,predict/255.0,0,1);
                        draw_cell(.4);
                    }
                }
            }
            glEnd();
        }

        int Region_texture(Region *region,GLuint tid)
        {
            int i;
            Interface *interface;
            
            if (region) ZLOOP(i,INTERFACES) if (i==INTRA)
            {
                interface=&htm.region[r].interface[i];
            
                glColor4f(1,1,1,1);
                if (interface && interface->output && interface->output->size.vol)
                {
                    GLubyte map[interface->output->size.x][interface->breadth][interface->output->size.y][interface->depth][4];
                    D3 opos={{},0,0,0};
                    int d,s;
                    int active,predict,perm;
                    float x,y,z,w,h;
                    
                    BZERO(map);
                    ZLOOP(opos.x,interface->output->size.x) ZLOOP(opos.y,interface->output->size.y)
                    {
                        (void) DIM3D(opos.v,interface->output->size.v);
                        ZLOOP(d,interface->breadth) ZLOOP(s,interface->depth)
                        {
                            active=interface->output->active[opos.vol];
                            predict=interface->output->predicted[opos.vol];
                            perm=interface->dendrites[opos.vol].dendrite[d].synapse[s].permanence;
                            map[opos.x][d][opos.y][s][0]=(GLubyte) active;
                            map[opos.x][d][opos.y][s][1]=(GLubyte) predict;
                            map[opos.x][d][opos.y][s][2]=(GLubyte) (perm>PTHRESH?perm:0);
                            map[opos.x][d][opos.y][s][3]=(GLubyte) perm;
                        }
                    }
                    
                    glPixelStorei(GL_UNPACK_ALIGNMENT, 0);
                    glBindTexture(GL_TEXTURE_2D,tid);
                
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,GL_NEAREST);
                    glTexImage2D(GL_TEXTURE_2D,
                                 0,
                                 GL_RGBA,
                                 interface->output->size.y*interface->depth,
                                 interface->output->size.x*interface->breadth,
                                 0,
                                 GL_RGBA,
                                 GL_UNSIGNED_BYTE, 
                                 map);
                
                    x=interface->output->position.x-.5;
                    y=interface->output->position.y-.5;
                    z=interface->output->position.z;
                    w=interface->output->size.x+.5;
                    h=interface->output->size.y+.5;
                
                    glEnable(GL_TEXTURE_2D);
                    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);
                    glBindTexture(GL_TEXTURE_2D, tid);
                    glBegin(GL_QUADS);
                    glTexCoord2f(0.0, 0.0); glVertex3f(x,y,z);
                    glTexCoord2f(0.0, 1.0); glVertex3f(x+w,y,z);
                    glTexCoord2f(1.0, 1.0); glVertex3f(x+w,y+h,z);
                    glTexCoord2f(1.0, 0.0); glVertex3f(x,y+h,z);
                    glEnd();
                    glDisable(GL_TEXTURE_2D);
                }
            }
        }
        
        int Interface_display(Interface *interface)
        {
            fvec jitter={{},0,0,0};
            fvec scale={{},0,0,0.05};
            fvec vertex;
            int axis;
            static int show=1;
            
            int synapse_op(D3 *ipos,D3 *opos,int dendrite,int synapse)
            {
                int active=interface->output->active[opos->vol];
                int predict=interface->output->predicted[opos->vol];
                if (active || predict)
                {
                    int perm=interface->dendrites[opos->vol].dendrite[dendrite].synapse[synapse].permanence;
                    
                    if (synapse==0)
                    {
                        show=(i==FEEDFWD || active || predict);

                        glBegin(GL_LINE_STRIP);
                        ZLOOP(axis,3) vertex.v[axis]=opos->v[axis]+interface->output->position.v[axis];
                        glColor4f(active/255.0,predict/255.0,0,1);
                        if (show && show_risers)
                            glVertex3fv(vertex.v);
                    }
                    
                    if (CLIP3D(ipos->v,interface->input->size.v))
                    {
                        glColor4f(active/255.0,predict/255.0,perm>PTHRESH?((perm-PTHRESH)/(float) PTHRESH):0,perm/255.0);
                        ZLOOP(axis,3) vertex.v[axis]=ipos->v[axis]+(opos->v[axis]+jitter.v[axis])*scale.v[axis]+interface->input->position.v[axis];
                        if (show)
                            glVertex3fv(vertex.v);
                    }
        
                    if (synapse==interface->depth-1)
                        glEnd();
                }
    
                return 0;
            }

            if (interface->output && interface->output->size.vol)
            {
                jitter.x = interface->output->position.x;
                scale.x = .5/interface->output->size.x;
                jitter.y = interface->output->position.y;
                scale.y = .5/interface->output->size.y;
                return Interface_traverse(interface,synapse_op);
            }
            else return 0;
        }

        int Scoring_display(Region *region)
        {
            D3 pos;
            float z;

            void vertex()
            {
                (void) DIM3D(pos.v,region->states.size.v);
                z=pos.z+region->states.position.z;
                if (show_scores) z+=region->states.score[pos.vol];
                if (show_suppression) z-=region->states.suppression[pos.vol];
                glVertex3f(region->states.position.x+pos.x,region->states.position.x+pos.y,z);
            }
            
            ZLOOP(pos.z,region->states.size.z)
            {
                switch(pos.z)
                {
                    default:
                    case 0: glColor4f(.5,.5,.5,1); break;
                    case 1: glColor4f(1,0,0,1); break;
                    case 2: glColor4f(0,1,0,1); break;
                    case 3: glColor4f(0,0,1,1); break;
                }
                
                ZLOOP(pos.x,region->states.size.x)
                {
                    glBegin(GL_LINE_STRIP);
                    ZLOOP(pos.y,region->states.size.y) vertex();
                    glEnd();
                }
                ZLOOP(pos.y,region->states.size.y)
                {
                    glBegin(GL_LINE_STRIP);
                    ZLOOP(pos.x,region->states.size.x) vertex();
                    glEnd();
                }
            }

            glFlush();
        }
            
        int DendriteMap_display()
        {
            int d,s,axis;
            ivec p,z={{},0,0,0};
            DendriteMap map;
            int dendrites=DENDRITE_CACHE;
    
            glColor4f(1,1,1,.01);
            ZLOOP(d,dendrites)
            {
                map=dendrites==DENDRITE_CACHE?DENDRITEMAP(d):DENDRITEMAP(NRAND(gseed)>>12);
                p=z;
                glBegin(GL_LINE_STRIP);
                glVertex3iv(p.v);
                ZLOOP(s,SYNAPSES)
                {
                    ZLOOP(axis,2) p.v[axis]+=map.offset[s].v[axis]; // x/y!
                    p.v[2]=map.offset[s].v[2]%4; // z!
                    glVertex3iv(p.v);
                }
                glEnd();
                glFlush();

            }
        }

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        if (show_cells) ZLOOP(r,htm.regions) Region_display(&htm.region[r]);
        if (show_scores || show_suppression) ZLOOP(r,htm.regions) Scoring_display(&htm.region[r]);
        if (show_tex) ZLOOP(r,htm.regions) Region_texture(&htm.region[r],1);
        glDepthMask(GL_FALSE);
        if (show_dendrites) ZLOOP(r,htm.regions) ZLOOP(i,INTERFACES) Interface_display(&htm.region[r].interface[i]);
        glDepthMask(GL_TRUE);

        if (show_map)
            DendriteMap_display();

        glutSwapBuffers();
    }
 
    void reshape(int w,int h)
    {
        float r=((float) w/(float) h);

        gwidth=w;
        gheight=h;
        glViewport(0,0,(GLsizei) w,(GLsizei) h);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        if (w>h) glFrustum (zoom*-r,zoom*r,-zoom,zoom,0.5,500.0);
        else     glFrustum (-zoom,zoom,zoom/-r,zoom/r,0.5,500.0);


        glMatrixMode(GL_MODELVIEW);
    }
    
    void keyboard(unsigned char key,int x,int y)
    {
        switch (key)
        {
            case 'q': case 27: exit(0); // esc
            case 'u': Htm_update(&htm);
            case 'c': show_cells=!show_cells; break;
            case 'd': show_dendrites=!show_dendrites; break;
            case 'm': show_map=!show_map; break;
            case 's': show_scores=!show_scores; break;
            case 'S': show_suppression=!show_suppression; break;
            case 'r': show_risers=!show_risers; break;
            case 'p': show_predictions=!show_predictions; break;
            case 't': show_tex=!show_tex; break;
            case 'h': hide_input=!hide_input; break;
            case 'g': do_generative=!do_generative; break;
        }
    }
    
    void mouse(int button,int state,int x,int y)
    {
        mousestate[button]=!state;
        mousepos[0]=x;
        mousepos[1]=y;
        if (state==0) switch (button)
        {
            case 3: glScalef(0.9,0.9,0.9); break;
            case 4: glScalef(1.1,1.1,1.1); break;
        }
        glutPostRedisplay();
    }

    void motion(int x,int y)
    {
        if (mousestate[1])
        {
            glTranslatef((x-mousepos[0])*.03,0,0);
            glTranslatef(0,0,-(y-mousepos[1])*.03);
            glutPostRedisplay();
        }
        else
        {
            glRotatef((x-mousepos[0])*.03,0,0,1);
            glRotatef((y-mousepos[1])*.03,1,0,0);
            glutPostRedisplay();
        }
        mousepos[0]=x;
        mousepos[1]=y;
    }
    
    void idle()
    {
        Htm_update(&htm);
        glutPostRedisplay();
    }
    
    void menuselect(int id)
    {
        switch (id)
        {
            case 0: exit(0); break;
        }
    }
    
    int menu(void)
    {
        int menu=glutCreateMenu(menuselect);
        glutAddMenuEntry("Exit demo\tEsc",0);
        return menu;

    }
    
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DEPTH | GLUT_DOUBLE | GLUT_ACCUM | GLUT_ALPHA | GLUT_RGBA | GLUT_STENCIL);

    glutInitWindowPosition(100,100);
    glutInitWindowSize(gwidth,gheight);
    glutCreateWindow("HTM");
    
    glutDisplayFunc(display);
    glutReshapeFunc(reshape);
    glutKeyboardFunc(keyboard);
    glutMouseFunc(mouse);
    glutMotionFunc(motion);
    menu();
    glutAttachMenu(GLUT_RIGHT_BUTTON);
    glutIdleFunc(idle);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_POINT_SMOOTH);
    glEnable(GL_LINE_SMOOTH);
    glEnable(GL_POLYGON_SMOOTH);
    glShadeModel(GL_SMOOTH);
    
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA); // transparency
    //glBlendFunc(GL_SRC_ALPHA_SATURATE,GL_ONE); // back-to-front compositing

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glutSwapBuffers();
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glutSwapBuffers();
    
    reshape(gwidth,gheight);
    glLoadIdentity();
    gluLookAt(camera[0],camera[1],camera[2],
              center[0],center[1],center[2],
              viewup[0],viewup[1],viewup[2]);
    
    glutMainLoop();
    
    return 0;
}
