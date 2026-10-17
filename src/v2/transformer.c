#include "transformer.h"
#include "intent_weights.h"
#include <math.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#define D 24
#define T 24
static void linear(const float *x,const float *w,const float *b,int in,int out,float *y) {
    for(int i=0;i<out;i++) {double sum=b?b[i]:0;for(int j=0;j<in;j++)sum+=w[i*in+j]*x[j];y[i]=(float)sum;}
}
const char *ct_label(int i) {static const char *s[]={"R","C","L","D","LED","J","V","Q","U","UNKNOWN"};return i>=0&&i<10?s[i]:"UNKNOWN";}
CtResult ct_classify(const char *s) {
    CtResult r={9,0,0};if(!s||strlen(s)>2048)return r;
    int ids[T],n=0,unknown=0;char word[64];
    while(*s) {
        if(!isalpha((unsigned char)*s)) {s++;continue;}
        size_t len=0;while(isalpha((unsigned char)*s)){if(len<sizeof(word)-1)word[len++]=(char)tolower((unsigned char)*s);s++;}word[len]=0;
        if(n==T)return r;
        int id=1;for(int i=2;i<CT_VOCAB;i++)if(!strcmp(word,ct_vocab[i])){id=i;break;}
        ids[n++]=id;if(id==1)unknown++;
    }
    if(n==0||unknown)return r; /* Out-of-vocabulary is an explicit abstention. */
    /* Reject mixed component requests before inference. The model never chooses
       which of several named devices the user "really" meant. */
    int roles[9]={0};
    const char *anchors[]={"|resistor|resistance|resistive|ohmic|","|capacitor|capacitance|","|inductor|inductance|choke|coil|","|diode|rectifier|schottky|","|led|emitting|","|connector|header|terminal|socket|","|voltage|supply|battery|","|transistor|mosfet|bjt|","|microcontroller|opamp|amplifier|chip|integrated|"};
    for(int i=0;i<n;i++) {char token[80];snprintf(token,sizeof token,"|%s|",ct_vocab[ids[i]]);for(int j=0;j<9;j++)if(strstr(anchors[j],token))roles[j]=1;}
    if(roles[4])roles[3]=0; /* "light emitting diode" is one LED role. */
    int role_count=0;for(int i=0;i<9;i++)role_count+=roles[i];if(role_count>1)return r;

    float z[T][D],q[T][D],k[T][D],v[T][D],h[T][D],pool[D]={0};
    for(int i=0;i<n;i++) {
        memcpy(z[i],ct_emb_weight+ids[i]*D,D*sizeof(float));
        linear(z[i],ct_q_weight,0,D,D,q[i]);linear(z[i],ct_k_weight,0,D,D,k[i]);linear(z[i],ct_v_weight,0,D,D,v[i]);
    }
    for(int i=0;i<n;i++) {
        double scores[T],max=-1e30,total=0;
        for(int j=0;j<n;j++){double sum=0;for(int d=0;d<D;d++)sum+=q[i][d]*k[j][d];scores[j]=sum/sqrt(D);if(scores[j]>max)max=scores[j];}
        for(int j=0;j<n;j++){scores[j]=exp(scores[j]-max);total+=scores[j];}
        float context[D]={0},o[D],ff[48],back[D];
        for(int j=0;j<n;j++)for(int d=0;d<D;d++)context[d]+=(float)(scores[j]/total)*v[j][d];
        linear(context,ct_o_weight,0,D,D,o);
        for(int d=0;d<D;d++)h[i][d]=z[i][d]+o[d];
        linear(h[i],ct_f1_weight,ct_f1_bias,D,48,ff);for(int d=0;d<48;d++)if(ff[d]<0)ff[d]=0;
        linear(ff,ct_f2_weight,ct_f2_bias,48,D,back);
        for(int d=0;d<D;d++)pool[d]+=(h[i][d]+back[d])/n;
    }
    float logits[10];linear(pool,ct_cls_weight,ct_cls_bias,D,10,logits);
    int best=0;for(int i=1;i<10;i++)if(logits[i]>logits[best])best=i;
    double total=0;for(int i=0;i<10;i++)total+=exp(logits[i]-logits[best]);
    r.label=best;r.confidence=1/total;r.accepted=best!=9&&r.confidence>=.90;return r;
}
