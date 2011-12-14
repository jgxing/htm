#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/freeglut.h>

#define DENDRITE_CACHE 0x1000
#define DENDRITES 6
#define SYNAPSES 6

#define SYNAPSE_THRESH 2
#define DENDRITE_THRESH 2
#define ACTIVE_THRESH 1
#define INIT_PERMANENCE_RANGE 20
#define IS_ACTIVE 0x80
#define WAS_ACTIVE 0x7f
#define SYNAPSE_ADJUSTMENT 5
#define MAX_PERMANENCE 120
#define MIN_PERMANENCE -120


#define MIN(x,y) ((x)<(y)?(x):(y))
#define MAX(x,y) ((x)>(y)?(x):(y))
#define ABS(x) ((x)<0?(-x):(x))
#define BZERO(x) bzero((&x),sizeof(x))

typedef struct { unsigned short s[0]; void *d1; void *d2; } Seed;
#define RESEED(seed,ptr) (seed.d1=seed.d2=ptr,seed.s)
#define LRAND(seed) (nrand48(seed.s))
#define DRAND(seed) (erand48(seed.s))


typedef struct { int v[0]; int x,y,z,vol; } D3;
#define VOL3V(i) ((i)[3]=(i)[0]*(i)[1]*(i)[2])

#define DIM3(x,y,z,d) ((x)*(d)[1]*(d)[2] + (y)*(d)[2] + (z))
#define DIM3V(i,d) (((i)[3]=DIM3((i)[0],(i)[1],(i)[2],(d))),((i)[3]<0?-1:((i)[3]>(d)[3]?-1:(i)[3])))
#define CLIP3V(i,d) (DIM3V((i),(d)),(i)[0]>=0 && (i)[0]<(d)[0] && (i)[1]>=0 && (i)[1]<(d)[1] && (i)[2]>=0 && (i)[2]<(d)[2])

#define LOOP(i,from,to) for ((i)=(from);(i)<(to);(i)++)
#define ZLOOP(i,lim) LOOP((i),0,lim)
#define LOOPD3(iv,fromv,tov) LOOP((iv)[0],(fromv)[0],(tov)[0]) LOOP((iv)[1],(fromv)[1],(tov)[1]) LOOP((iv)[2],(fromv)[2],(tov)[2])
#define ZLOOPD3(iv,limv) LOOP((iv)[0],0,(limv)[0]) LOOP((iv)[1],0,(limv)[1]) LOOP((iv)[2],0,(limv)[2])

typedef struct { int   v[3]; } ivec;
typedef struct { char  v[3]; } cvec;
typedef struct { float v[3]; } fvec;

typedef struct { cvec offset[SYNAPSES]; } DendriteMap;

DendriteMap gDendriteMap[DENDRITE_CACHE];
int cycles=0;
Seed gseed;
#define DENDRITEMAP(i) (gDendriteMap[(i)%DENDRITE_CACHE])

int show_cells=1;
int show_dendrites=1;
int show_map=0;
int show_scores=0;
int show_suppression=0;


typedef struct
{
    char permanence;
    unsigned char sensitivity;
} Synapse;

typedef struct
{
    Synapse synapse[SYNAPSES];
    unsigned char score;
    unsigned char bias;
} Dendrite;

typedef struct
{
    Dendrite dendrite[DENDRITES];
} Dendrites;
    
typedef struct
{
    D3 size;
    fvec position;
    unsigned char *state; // msb represents current state, 50% decay per tick
    float *score;
    float *suppression;
} StateMap;

typedef struct
{
    StateMap *input;
    StateMap *output;
    Dendrites *dendrites;
} Interface;

enum { FEEDFWD,INTRA,FEEDBACK,INTERFACES }; // inter-region interfaces

typedef struct
{
    StateMap states;
    Interface interface[INTERFACES];
} Region;



void DendriteMap_init()
{
    int i;
    
    void generate()
    {
        int synapse;
        unsigned long long r=LRAND(gseed);
        int v[]={1,(r&0x10000000)?1:-1};
        int xx=(r&0x2000000)?1:-1; // flip l/r
        int xy=(r&0x4000000)?1:0; // switch axes
        int mx[]={6,2};

        r<<=31;
        r|=LRAND(gseed);
        BZERO(DENDRITEMAP(i));
        
        LOOP(synapse,1,SYNAPSES)
        {
            if ((r&mx[v[0]]) && !(v[0]=!v[0]) && (r&1)) v[1]=-v[1];
            //if (r&mx[v[0]] && (v[0]=!v[0]) && r&1) v[1]=-v[1];
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
    VOL3V(size->v); // assigns to vol
    map->size=*size;
    map->position=*position;
    map->state=malloc(size->vol*sizeof(map->state[0]));
    map->score=malloc(size->vol*sizeof(map->score[0]));
    map->suppression=malloc(size->vol*sizeof(map->suppression[0]));
    bzero(map->state,size->vol*sizeof(map->state[0]));
    bzero(map->score,size->vol*sizeof(map->score[0]));
    bzero(map->suppression,size->vol*sizeof(map->suppression[0]));
    return 0;
}

int Interface_init(Interface *interface,StateMap *input,StateMap *output,int sensitivity)
{
    int i,d,s;

    int dendrites;

    if (!interface || !output || !input) return !0;
    interface->input=input;
    interface->output=output;
    interface->dendrites=malloc(output->size.vol * sizeof(Dendrites));
    
    ZLOOP(i,output->size.vol) ZLOOP(d,DENDRITES) ZLOOP(s,SYNAPSES)
    {
        interface->dendrites[i].dendrite[d].synapse[s].permanence=LRAND(gseed)%INIT_PERMANENCE_RANGE-(INIT_PERMANENCE_RANGE>>1);
        interface->dendrites[i].dendrite[d].synapse[s].sensitivity=sensitivity?sensitivity:~(0xff>>(LRAND(gseed)&7)); // 2^n, n=1 through 8
    }
    return 0;
}

typedef int (*Synapse_op)(Interface *interface,D3 *ipos,D3 *opos,int dendrite,int synapse);

int Interface_traverse(Interface *interface,Synapse_op op)
{
    Seed seed;
    D3 ipos,opos;
    fvec delta;
    int d,s,axis;
    DendriteMap map;
    Synapse *syn;
    int i;

    if (!interface || !interface->input || !interface->output) return !0;
    
    ZLOOP(axis,3) delta.v[axis]=((float) interface->input->size.v[axis])/((float) interface->output->size.v[axis]);
    RESEED(seed,interface); // reseed when traversing an interface's dendrites
    
    ZLOOPD3(opos.v,interface->output->size.v)
    {
        // score synapses
        (void) DIM3V(opos.v,interface->output->size.v); // populate opos.vol
        ZLOOP(d,DENDRITES)
        {
            ZLOOP(axis,3) ipos.v[axis]=(int) (opos.v[axis]*delta.v[axis]);
            map=DENDRITEMAP(LRAND(seed)>>12);
            ZLOOP(s,SYNAPSES)
            {
                ZLOOP(axis,2) ipos.v[axis]+=map.offset[s].v[axis]; // x/y!
                ipos.z=map.offset[s].v[2]%interface->input->size.z; // z!
                if (op(interface,&ipos,&opos,d,s))
                    goto done;
            }
        }
    }
 done:
    return 0;
}
                
int Synapse_score(Interface *interface,D3 *ipos,D3 *opos,int dendrite,int synapse)
{
    if (synapse==0)
        interface->dendrites[opos->vol].dendrite[dendrite].score=0;
    
    if (interface->input==interface->output && synapse==0) return 0; // don't let state use itself as input
    if (CLIP3V(ipos->v,interface->input->size.v))
    {
        Synapse *syn=&interface->dendrites[opos->vol].dendrite[dendrite].synapse[synapse];
        if ((syn->sensitivity & interface->input->state[ipos->vol]) && (syn->permanence > SYNAPSE_THRESH))
            interface->dendrites[opos->vol].dendrite[dendrite].score+=1;
    }

    if (synapse==SYNAPSES-1 && interface->dendrites[opos->vol].dendrite[dendrite].score > DENDRITE_THRESH)
        interface->output->score[opos->vol]+=1;
        
    return 0;
}

int Synapse_suppress(Interface *interface,D3 *ipos,D3 *opos,int dendrite,int synapse)
{
    if (interface->input==interface->output && synapse==0) return 0; // don't let state use itself as input
    if (CLIP3V(ipos->v,interface->input->size.v))
        interface->input->suppression[ipos->vol] += (interface->output->score[opos->vol]*(((float) SYNAPSES-synapse)/(float) SYNAPSES));
    return 0;
}

int Synapse_train(Interface *interface,D3 *ipos,D3 *opos,int dendrite,int synapse)
{
    if (interface->input==interface->output && synapse==0) return 0; // don't let state use itself as input
    if (!CLIP3V(ipos->v,interface->input->size.v))
    {
        Synapse *syn=&interface->dendrites[opos->vol].dendrite[dendrite].synapse[synapse];
        if (interface->output->state[opos->vol]&(IS_ACTIVE|WAS_ACTIVE))
        {
            if (interface->input->state[ipos->vol])
                syn->permanence+=SYNAPSE_ADJUSTMENT;
            else
                syn->permanence-=SYNAPSE_ADJUSTMENT;
        }
        syn->permanence=MIN(syn->permanence,MAX_PERMANENCE);
        syn->permanence=MAX(syn->permanence,MIN_PERMANENCE);
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


int Region_update(Region *region)
{
    int i,j;
    
    if (!region) return !0;

    if (region->interface[FEEDFWD].input) // spatial/temporal pooler
    {
        int x,y,z;
        int interface;
        
        // age states
        ZLOOP(i,region->states.size.vol)
        {
            region->states.state[i]>>=1;
            region->states.score[i]=0;
            region->states.suppression[i]=0;
        }

        // propagate inputs
        ZLOOP(i,INTERFACES) Interface_traverse(&region->interface[i],Synapse_score);
        
        // suppress columns, i.e select winners
        Interface_traverse(&region->interface[INTRA],Synapse_suppress);
        
        // activate sufficiently post-supporession stimulated cells;
        ZLOOP(i,region->states.size.vol) if ((region->states.score[i]-region->states.suppression[i]) > ACTIVE_THRESH)
            region->states.state[i]|=IS_ACTIVE;

        // update synapses
        ZLOOP(interface,INTERFACES) Interface_traverse(&region->interface[interface],Synapse_train);
    }
    else // an input layer... read from stdin
    {
        D3 p,a={{},1,1,0,0},b={{},6,6,1,0},c={{},7,7,1,0};
        static int offset=0;
        
        //printf("  Reading %d bytes:\n",region->states.size.vol);
        //ZLOOP(i,region->states.size.vol) region->states.state[i]=getchar();
        ZLOOP(i,region->states.size.vol) region->states.state[i]=0x00;
        ZLOOPD3(p.v,c.v) region->states.state[(DIM3V(p.v,region->states.size.v)+offset)%region->states.size.vol]=0xff;
        LOOPD3(p.v,a.v,b.v) region->states.state[(DIM3V(p.v,region->states.size.v)+offset)%region->states.size.vol]=0x00;
        offset++;
        //printf("  Done!\n");
    }
    
    return 0;
}


typedef struct 
{
    D3 size;
    fvec position;

    int lowerlayer; // relative offset from this layer to it's lower-layer
    //D3 ul,lr; // portion of lower layer this layer reads from
} RegionDesc;

typedef struct
{
    int regions;
    Region *region;
} Htm;


int Htm_init(Htm *htm,RegionDesc *rd,int regions)
{
    int r,ll;
    if (!htm || !regions || !rd) return !0;

    DendriteMap_init();

    htm->regions=regions;
    htm->region=malloc(sizeof(Region)*regions);
    ZLOOP(r,regions) Region_init(&htm->region[r],&rd[r].size,&rd[r].position);

    ZLOOP(r,regions) if ((ll=rd[r].lowerlayer))
    {
        if (rd[r-ll].lowerlayer)
            Interface_init(&htm->region[r].interface[FEEDFWD],&htm->region[r-ll].states,&htm->region[r].states,IS_ACTIVE);
        else // bottom layers output sensory info, not cell states
            Interface_init(&htm->region[r].interface[FEEDFWD],&htm->region[r-ll].states,&htm->region[r].states,0);
        
        Interface_init(&htm->region[r].interface[INTRA],&htm->region[r].states,&htm->region[r].states,WAS_ACTIVE);
        //Interface_init(&htm->region[r-ll].interface[FEEDBACK],&htm->region[r].states,&htm->region[r-ll].states,WAS_ACTIVE);

    }
}

int Htm_update(Htm *htm)
{
    int r;
    if (!htm) return !0;
    ZLOOP(r,htm->regions) Region_update(&htm->region[r]);
    cycles++;

}

int Synapse_display(Interface *interface,D3 *ipos,D3 *opos,int dendrite,int synapse)
{
    fvec vertex;
    int axis;
    
    int state=interface->output->state[opos->vol];
    if (state)
    {
        if (synapse==0)
        {
            if (dendrite==0 )
            {
                int state=interface->output->state[opos->vol];
                glColor4f(state&IS_ACTIVE?1.0:0.0,state&WAS_ACTIVE?1.0:0.0,0,state/255.0);
            }
            glBegin(GL_LINE_STRIP);
            ZLOOP(axis,3) vertex.v[axis]=opos->v[axis]+interface->output->position.v[axis];
            glVertex3fv(vertex.v);
        }
        
        if (CLIP3V(ipos->v,interface->input->size.v))
        {
            ZLOOP(axis,3) vertex.v[axis]=ipos->v[axis]+interface->input->position.v[axis];
            glVertex3fv(vertex.v);
        }
        
        if (synapse==SYNAPSES-1)
            glEnd();
    }
    
    return 0;
}

 
Region_display(Region *region)
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
        (void) DIM3V(opos.v,region->states.size.v);
        ZLOOP(axis,3) vertex.v[axis]=opos.v[axis]+region->states.position.v[axis];
        if (show_cells)
        {
            if ((state=region->states.state[opos.vol]))
            {
                glColor4f(state&IS_ACTIVE?1.0:0.0,state&WAS_ACTIVE?1.0:0.0,0,state/255.0);
                draw_cell(.4);
            }
        }
        if (show_scores)
        {
            score=region->states.score[opos.vol];
            glColor4f(0.0,0.0,score/16.0,1.0);
            draw_cell(.2);
        }
        if (show_suppression)
        {
            suppression=region->states.suppression[opos.vol];
            glColor4f(0.0,0.0,suppression/16.0,1.0);
            draw_cell(.3);
        }
    }
    glEnd();
}

int DendriteMap_display()
{
    int d,s,axis;
    ivec p,z={{0,0,0}};
    DendriteMap map;
    int dendrites=DENDRITE_CACHE;
    
    glColor4f(1,1,1,.01);
    ZLOOP(d,dendrites)
    {
        map=dendrites==DENDRITE_CACHE?DENDRITEMAP(d):DENDRITEMAP(LRAND(gseed)>>12);
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



int main(int argc, char **argv)
{
    int gwidth=400,gheight=400;
    
    float camera[] = { 15,-25,5 };
    float center[] = { 9,0,6 };

    float viewup[] = { 0,0,1 };
    float zoom=.20;

    int mousestate[6]={0,0,0,0,0,0};
    int mousepos[2]={0,0};

    
    Htm htm;
    RegionDesc rd[]= {
        {{{},16,16,1,0 },{0,0, 0},0},
        {{{},16,16,4,0 },{0,0,6},1},
        {{{},16,16,4,0 },{0,0,12},1}
    };
        
    Htm_init(&htm,rd,3);

    
    void display()
    {
        float x=0,y=0;
        int i=0;
        int region,interface;
        
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        if (show_cells || show_scores || show_suppression)
            ZLOOP(region,htm.regions) Region_display(&htm.region[region]);

        glDepthMask(GL_FALSE);
        if (show_dendrites)
            ZLOOP(region,htm.regions) ZLOOP(interface,INTERFACES) Interface_traverse(&htm.region[region].interface[interface],Synapse_display);
        glDepthMask(GL_TRUE);

        if (show_map)
            DendriteMap_display();
        
        //glFlush();

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
            case 'r': break; // redisplay
        }
        glutPostRedisplay();

    }
    
    void mouse(int button,int state,int x,int y)
    {
        mousestate[button]=!state;
        mousepos[0]=x;
        mousepos[1]=y;
        if (state==0) switch (button)
        {
            case 3: glScalef(1.1,1.1,1.1); break;
            case 4: glScalef(0.9,0.9,0.9); break;
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
        glutPostRedisplay();
        Htm_update(&htm);

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
