#include "yolo_layer.h"
#include "activations.h"
#include "blas.h"
#include "box.h"
#include "cuda.h"
#include "utils.h"

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

layer make_yolo_layer(int batch, int w, int h, int n, int total, int *mask, int classes)
{
    int i;
    layer l = {0};
    l.type = YOLO;

    l.n = n;// mask's number
    l.total = total;// total anchor box number
    l.batch = batch; // batch size ( 4 )
    l.h = h;
    l.w = w;
    l.c = n*(classes + 4 + 1); // anchor box num * ( classes + 4(box offsets) + 1 (objectness predict)))
    l.out_w = l.w;
    l.out_h = l.h;
    l.out_c = l.c;
    l.classes = classes;
    l.cost = calloc(1, sizeof(float));
    l.biases = calloc(total*2, sizeof(float));
    if(mask) l.mask = mask;
    else{
        l.mask = calloc(n, sizeof(int));
        for(i = 0; i < n; ++i){
            l.mask[i] = i;
        }
    }
    l.bias_updates = calloc(n*2, sizeof(float));
    l.outputs = h*w*n*(classes + 4 + 1);
    l.inputs = l.outputs;
    l.truths = 90*(4 + 1);
    l.delta = calloc(batch*l.outputs, sizeof(float));
    l.output = calloc(batch*l.outputs, sizeof(float));
    for(i = 0; i < total*2; ++i){
        l.biases[i] = .5;
    }

    l.forward = forward_yolo_layer;
    l.backward = backward_yolo_layer;
#ifdef GPU
    l.forward_gpu = forward_yolo_layer_gpu;
    l.backward_gpu = backward_yolo_layer_gpu;
    l.output_gpu = cuda_make_array(l.output, batch*l.outputs);
    l.delta_gpu = cuda_make_array(l.delta, batch*l.outputs);
#endif

    fprintf(stderr, "yolo");
    printf("l.n = %d , l.c = %d\n",l.n,l.c);
    srand(0);

    return l;
}

void resize_yolo_layer(layer *l, int w, int h)
{
    l->w = w;
    l->h = h;

    l->outputs = h*w*l->n*(l->classes + 4 + 1);
    l->inputs = l->outputs;

    l->output = realloc(l->output, l->batch*l->outputs*sizeof(float));
    l->delta = realloc(l->delta, l->batch*l->outputs*sizeof(float));

#ifdef GPU
    cuda_free(l->delta_gpu);
    cuda_free(l->output_gpu);

    l->delta_gpu =     cuda_make_array(l->delta, l->batch*l->outputs);
    l->output_gpu =    cuda_make_array(l->output, l->batch*l->outputs);
#endif
}

box get_yolo_box(float *x, float *biases, int n, int index, int i, int j, int lw, int lh, int w, int h, int stride)
{ // x = l.output (l.outw*l.outh*l.outn), index = box_index(각 배치의 각 픽셀의 정보)
//(l.output, l.biases, l.mask[n], box_index, i, j, l.w, l.h, net.w, net.h, l.w*l.h);
// i = 0 ~ width , j = 0 ~ height
    box b;
    /*x[index+0*stride] = tx / x[index+1*stride] = ty
      x[index+2*stride] = tw / x[index+3*stride] = th
    */
    b.x = (i + x[index + 0*stride]) / lw; // stirde = l.w*l.h
    b.y = (j + x[index + 1*stride]) / lh;
    b.w = exp(x[index + 2*stride]) * biases[2*n]   / w; // exp() = 지수 제곱 biases have anchor's width and height
    b.h = exp(x[index + 3*stride]) * biases[2*n+1] / h;
    //printf("biases.w = %f , biases.h = %f\n",biases[2*n] ,biases[2*n+1] );
    // biases[2*n] = anchor box's width , biases[2*n+1] = anchor box's height
    return b;
}

float delta_yolo_box(box truth, float *x, float *biases, int n, int index, int i, int j, int lw, int lh, int w, int h, float *delta, float scale, int stride)
{ // delta의 의미
    //(truth, l.output, l.biases, l.mask[n], box_index, i, j, l.w, l.h, net.w, net.h, l.delta, (2-truth.w*truth.h), l.w*l.h);
    box pred = get_yolo_box(x, biases, n, index, i, j, lw, lh, w, h, stride);
    //anchor box와 output을 사용하여 iou값 유추
    float iou = box_iou(pred, truth);
    //식확인하기 anchor box 와 bounding box 관련 식
    float tx = (truth.x*lw - i); // tx = truth.x - i(cx)
    float ty = (truth.y*lh - j); // ty = truth.y - j(cy)
    float tw = log(truth.w*w / biases[2*n]);
    float th = log(truth.h*h / biases[2*n + 1]);
    //printf("tx = %lf, ty = %lf, tw = %lf, th = %lf\n",tx,ty,tw,th);
    // x = output
    //stride = l.w*l.h
    //printf("scale = %lf\n",scale);
    //(2-truth.w*truth.h) = scale
    delta[index + 0*stride] = scale * (tx - x[index + 0*stride]);
    delta[index + 1*stride] = scale * (ty - x[index + 1*stride]);
    delta[index + 2*stride] = scale * (tw - x[index + 2*stride]);
    delta[index + 3*stride] = scale * (th - x[index + 3*stride]);
    return iou;
}


void delta_yolo_class(float *output, float *delta, int index, int class, int classes, int stride, float *avg_cat)
{//(l.output, l.delta, class_index, class, l.classes, l.w*l.h, 0);
    int n;
    if (delta[index]){ // objectscore이 있을 경우
        delta[index + stride*class] = 1 - output[index + stride*class];
        if(avg_cat) *avg_cat += output[index + stride*class]; // What
        return;
    }
    for(n = 0; n < classes; ++n){ // if delta[index] == 0 object score이 없는 경우에는
    // detection부분에 있어서는 없을경우에는 무시하지만 현재 학습 단계에서는 없을 경우에도 모든 객체 점수를 파악하여 
    //loss함수 계산하는데 사용하여야 하기 때문에 해당 반복문을 통해서 delta값을 저장함
        delta[index + stride*n] = ((n == class)?1 : 0) - output[index + stride*n];
        if(n == class && avg_cat) *avg_cat += output[index + stride*n];
    }
}
// 0.4 0.6 -> 0.16 , 0.36 --> 값이 0에 가까울수록 좋은 것?
static int entry_index(layer l, int batch, int location, int entry) // what is it??
{ // location = n*l.w*l.h + j*l.w + i = 해당 이미지의 위치
//(l, b, n*l.w*l.h + j*l.w + i, 4)
    int n =   location / (l.w*l.h); // 0 ~ 2 ( always )
    int loc = location % (l.w*l.h); // 0 ( always )
    //printf(" %d ",(batch*l.outputs + n*l.w*l.h*(4+l.classes+1) + entry*l.w*l.h + loc)/(l.w*l.h));
    return batch*l.outputs + n*l.w*l.h*(4+l.classes+1) + entry*l.w*l.h + loc;
    // 4 * width * height * filters(18) + n(0~2) * width * height *(5+classes(1)) + 0
    //n = anchor box number
    // (width*height)(72+(0~2))
}

void forward_yolo_layer(const layer l, network net)// forward_yolo_layer() function
{ // 공부하기
    int i,j,b,t,n;
    memcpy(l.output, net.input, l.outputs*l.batch*sizeof(float));

#ifndef GPU
    for (b = 0; b < l.batch; ++b){
        for(n = 0; n < l.n; ++n){
            int index = entry_index(l, b, n*l.w*l.h, 0);
            activate_array(l.output + index, 2*l.w*l.h, LOGISTIC);
            index = entry_index(l, b, n*l.w*l.h, 4);
            activate_array(l.output + index, (1+l.classes)*l.w*l.h, LOGISTIC);
        }
    }
#endif

    memset(l.delta, 0, l.outputs * l.batch * sizeof(float));
    if(!net.train) return;
    float avg_iou = 0;
    float recall = 0;
    float recall75 = 0;
    float avg_cat = 0;
    float avg_obj = 0;
    float avg_anyobj = 0;
    int count = 0;
    int class_count = 0;
    *(l.cost) = 0;
    //
    //printf("l.height = %d , l.width = %d , l.n = %d , l.filters = %d\n",l.h,l.w,l.n,l.c);
    //printf("net.truth = %d, net.truths = %d\n",net.truth,net.truths);
    for (b = 0; b < l.batch; ++b) { // batch(4) grid접근 방식
        for (j = 0; j < l.h; ++j) { // height
            for (i = 0; i < l.w; ++i) { // width
                for (n = 0; n < l.n; ++n) { // anchor's number = 3
                    int box_index = entry_index(l, b, n*l.w*l.h + j*l.w + i, 0); // 몇번째 grid cell인가
                    //printf(" box_index = %d ",box_index);
                    // 학습하는 이미지의 각각의 이미지를 따로 가져옴. 한번에 4개의 이미지를 읽기 때문에 batch또한 0번부터 3번까지 나눠서 정보를 가져옴
                    // n*l.w*l.h + j*l.w + i == 이미지 RGB의 모든 값을 가지는 1차원 배열 정보
                    box pred = get_yolo_box(l.output, l.biases, l.mask[n], box_index, i, j, l.w, l.h, net.w, net.h, l.w*l.h);
                    //3개의 anchor box 확인
                    float best_iou = 0;
                    int best_t = 0;
                    //for(t = 0 ; t < 1 ; ++t){
                    for(t = 0; t < l.max_boxes; ++t){ // 모든 객체들에 대해서 iou값을 확인
                        box truth = float_to_box(net.truth + t*(4 + 1) + b*l.truths, 1); //  b = 4
                        if(!truth.x) break;// net.truth + t*(4 + 1) + b*l.truths 이것이 의미하는 것은?
                        float iou = box_iou(pred, truth); // 예측과 실측에 대한 iou값 계산
                        if (iou > best_iou) { // 최대의 iou값만 남긴다
                            best_iou = iou; // 가장 높은 iou값을 가진 객체의 값을 저장
                            best_t = t; // test_t = 무슨 객체인지를 알려줌 여기서 t = 0 이면
                            // person을 뜻함
                        }
                    }
                    //하나의 cell에 대해서 모든 객체 점수들을 종합하여 최대의 값을 파악
                    int obj_index = entry_index(l, b, n*l.w*l.h + j*l.w + i, 4); // 해당 grid cell 위치에서 objectness값 확인
                    //printf("l.w = %d , l.h = %d, n = %d , loc = %d\n",l.w,l.h,(n*l.w*l.h + j*l.w + i)/(l.w*l.h),(n*l.w*l.h + j*l.w + i)%(l.w*l.h));
                    //printf("obj_index = %d\n",obj_index);
                    // objectness score정보를 가져오기 위한 obj_index
                    avg_anyobj += l.output[obj_index];
                    // 전체 평균값에 obj_index값을 증감
                    l.delta[obj_index] = 0 - l.output[obj_index];
                    //best_iou가 0.7보다 작은 경우 l.output[obj_index]는 0에 가까운 값이기에 0 - 로 시작
                    //하지만 1보다 큰 경우에는 l.output이 1에 근사한 값이기 때문에 1- 로 시작함
                    //printf("best_iou = %lf, l.ignore_thresh = %lf, l.truth_thresh = %lf,delta[obj_index] = %lf\n",best_iou,l.ignore_thresh,l.truth_thresh,l.delta[obj_index]);
                    //printf("best_iou = %f , best_t = %d, l.ignore_thresh = %f, l.truth_thresh = %f\n",best_iou,best_t,l.ignore_thresh,l.truth_thresh);
                    if (best_iou > l.ignore_thresh) { // best_iou > 0.7
                        //printf("111\n");
                        l.delta[obj_index] = 0;
                    }
                    if (best_iou > l.truth_thresh){ // best_iou > 1 예외처리 느낌으로 사용
                        //printf("222\n");
                        l.delta[obj_index] = 1 - l.output[obj_index]; // make l.delta = 0 ~ 1

                        int class = net.truth[best_t*(4 + 1) + b*l.truths + 4];
                        if (l.map) class = l.map[class];
                        int class_index = entry_index(l, b, n*l.w*l.h + j*l.w + i, 4 + 1);
                        delta_yolo_class(l.output, l.delta, class_index, class, l.classes, l.w*l.h, 0);
                        //현재 cell에서 가장 알맞은 anchor박스를 통하여 
                        box truth = float_to_box(net.truth + best_t*(4 + 1) + b*l.truths, 1);
                        delta_yolo_box(truth, l.output, l.biases, l.mask[n], box_index, i, j, l.w, l.h, net.w, net.h, l.delta, (2-truth.w*truth.h), l.w*l.h);
                    }
                } // end firth iteration
            }//end third iteration
        }//end second iteration
        //for(t = 0 ; t < 1 ; ++t){
        for(t = 0; t < l.max_boxes; ++t)
        { // 해당 반복문 공부
            box truth = float_to_box(net.truth + t*(4 + 1) + b*l.truths, 1);
            //net->truth의 값은 train부분에서 get_next_batch()함수 [이미지에 대한 정보를 가져오는 작업]에서
            //실측값(ground truth)를 다음 구조체 변수에 저장한다.
            //해당 작업은 if(y) memcpy(y+j*d.y.cols, d.y.vals[index], d.y.cols*sizeof(float));
            //와 같은 if문에서 처리
            //net->truth = net->truths*net->batch
            if(!truth.x) break;
            float best_iou = 0;
            int best_n = 0;
            //printf("truth.x = %lf, truth.y = %lf, l.w = %d , l.h = %d\n",truth.x,truth.y,l.w,l.h);
            i = (truth.x * l.w);
            j = (truth.y * l.h);
            //i,j = 실측값 중심점 좌표 학습 이미지를 resizing 시켰기 때문에 위치 재조정
            box truth_shift = truth;
            truth_shift.x = truth_shift.y = 0;
            for(n = 0; n < l.total; ++n){
                box pred = {0};
                pred.w = l.biases[2*n]/net.w;
                pred.h = l.biases[2*n+1]/net.h;
                //pred.w,h = anchor box's w,h
                //printf("pred.w = %lf, pred.h = %lf, pred.x = %lf, pred.y = %lf\n",pred.w,pred.h,pred.x,pred.y);
                //printf("truth_shift.w = %lf, truth_shift.h = %lf, truth_shift.x = %lf, truth_shift.y = %lf\n",truth_shift.w,truth_shift.h,truth_shift.x,truth_shift.y);
                float iou = box_iou(pred, truth_shift);
                //truth_shift는 x,y는 0 w,h는 실측값에서 가져온다
                //실측값과 anchor box의 iou
                if (iou > best_iou){
                    best_iou = iou;
                    best_n = n;// 제일 잘 맞는 anchor박스를 검출
                }
            }
            //
            int mask_n = int_index(l.mask, best_n, l.n); // best_n = 0 ~ 8
            //모든 anchor박스에 하는 이유는 다른 yolo에서도 똑같이 해당 함수를 사용하지만
            //해당 l.mask의 값에 따라서 사용할 수 있는 anchor박스는 제한적
            //따라서 첫번째 yolo 레이어에서는 6,7,8 anchor box에 대해서만 사용 가능
            //printf("mask_n = %d, best_n = %d, l.n = %d\n",mask_n,best_n,l.n);
            //mask_n = 0 ~ 2 
            /* 결과 예시
            utils.c 635 line
                mask_n = -1, best_n = 5, l.n = 3
                mask_n = -1, best_n = 5, l.n = 3
                mask_n = -1, best_n = 5, l.n = 3
                mask_n = -1, best_n = 3, l.n = 3
                mask_n = -1, best_n = 3, l.n = 3
                mask_n = -1, best_n = 7, l.n = 3
                mask_n = 1, best_n = 1, l.n = 3
                mask_n = -1, best_n = 5, l.n = 3
                mask_n = 1, best_n = 1, l.n = 3
                mask_n = -1, best_n = 5, l.n = 3
                mask_n = -1, best_n = 4, l.n = 3
              */
            //printf("i = %d , j = %d\n",i,j); // 실측값의 x,y좌표
            if(mask_n >= 0){ // find something
                int box_index = entry_index(l, b, mask_n*l.w*l.h + j*l.w + i, 0);
                //b = batch 사진 한장
                float iou = delta_yolo_box(truth, l.output, l.biases, best_n, box_index, i, j, l.w, l.h, net.w, net.h, l.delta, (2-truth.w*truth.h), l.w*l.h);
                //(box truth, float *x, float *biases, int n, int index, int i, int j, int lw, int lh, int w, int h, float *delta, float scale, int stride)

                int obj_index = entry_index(l, b, mask_n*l.w*l.h + j*l.w + i, 4);
                avg_obj += l.output[obj_index];
                l.delta[obj_index] = 1 - l.output[obj_index];
                // 1 = 정답 - l.output[] = 예측한 값, truth의 확률
                int class = net.truth[t*(4 + 1) + b*l.truths + 4];
                if (l.map) class = l.map[class];
                int class_index = entry_index(l, b, mask_n*l.w*l.h + j*l.w + i, 4 + 1);
                delta_yolo_class(l.output, l.delta, class_index, class, l.classes, l.w*l.h, &avg_cat);

                ++count;
                ++class_count;
                if(iou > .5) recall += 1;
                if(iou > .75) recall75 += 1;
                avg_iou += iou;
            }
        }//end t iteration
    }//end first iteration
    //printf("l.outputs = %d , l.batch = %d\n",l.outputs,l.batch);
    *(l.cost) = pow(mag_array(l.delta, l.outputs * l.batch), 2); // 중요
    printf("(Yolo)Region %d Avg IOU: %f, Class: %f, Obj: %f, No Obj: %f, .5R: %f, .75R: %f,  count: %d\n", net.index, avg_iou/count, avg_cat/class_count, avg_obj/count, avg_anyobj/(l.w*l.h*l.n*l.batch), recall/count, recall75/count, count);
}//end forward_yolo_layer() function

void backward_yolo_layer(const layer l, network net)
{
   axpy_cpu(l.batch*l.inputs, 1, l.delta, 1, net.delta, 1);
}

void correct_yolo_boxes(detection *dets, int n, int w, int h, int netw, int neth, int relative)
{
    int i;
    int new_w=0;
    int new_h=0;
    if (((float)netw/w) < ((float)neth/h)) { // image 크기 조정
        new_w = netw;
        new_h = (h * netw)/w;
    } else {
        new_h = neth;
        new_w = (w * neth)/h;
    }
    for (i = 0; i < n; ++i){
        box b = dets[i].bbox;
        b.x =  (b.x - (netw - new_w)/2./netw) / ((float)new_w/netw); 
        b.y =  (b.y - (neth - new_h)/2./neth) / ((float)new_h/neth); 
        b.w *= (float)netw/new_w;
        b.h *= (float)neth/new_h;
        if(!relative){ // relative = 1 따라서 다음 if문 사용 x 
            b.x *= w;
            b.w *= w;
            b.y *= h;
            b.h *= h;
        }
        dets[i].bbox = b;
    }
}

int yolo_num_detections(layer l, float thresh)
{//yolo_num_detection
    int i, n;
    int count = 0;
    for (i = 0; i < l.w*l.h; ++i){ // gird cell 접근
        for(n = 0; n < l.n; ++n){//anchor box접근
            int obj_index  = entry_index(l, 0, n*l.w*l.h + i, 4); // objectness값 추출
            if(l.output[obj_index] > thresh){ // 임계값보다 큰 경우 객체를 찾음
            //무슨 객체인지는 신경 x
                ++count; // 찾은 객체수를 증가
            }
        }
    }
    return count;
}

void avg_flipped_yolo(layer l) // 왜 flipped를 하는가 detection 단계에서 
{
    int i,j,n,z;
    float *flip = l.output + l.outputs;
    for (j = 0; j < l.h; ++j) {
        for (i = 0; i < l.w/2; ++i) {
            for (n = 0; n < l.n; ++n) {
                for(z = 0; z < l.classes + 4 + 1; ++z){
                    int i1 = z*l.w*l.h*l.n + n*l.w*l.h + j*l.w + i;
                    int i2 = z*l.w*l.h*l.n + n*l.w*l.h + j*l.w + (l.w - i - 1);
                    float swap = flip[i1];
                    flip[i1] = flip[i2];
                    flip[i2] = swap;
                    if(z == 0){
                        flip[i1] = -flip[i1];
                        flip[i2] = -flip[i2];
                    }
                }
            }
        }
    }
    for(i = 0; i < l.outputs; ++i){
        l.output[i] = (l.output[i] + flip[i])/2.;
    }
}

int get_yolo_detections(layer l, int w, int h, int netw, int neth, float thresh, int *map, int relative, detection *dets)
{ // detection시 yolo layer
    int i,j,n;
    float *predictions = l.output;
    if (l.batch == 2) avg_flipped_yolo(l);
    int count = 0;
    for (i = 0; i < l.w*l.h; ++i){ // 모든 grid cell에 대한 접근 
        int row = i / l.w; // 행에 대한 접근
        int col = i % l.w; // 열에 대한 접근
        //2중 for문이 아닌 1중 for문으로 처리하기 때문에 row,col이 다음과 같이 접근 가능 
        for(n = 0; n < l.n; ++n){ // anchor box접근
            int obj_index  = entry_index(l, 0, n*l.w*l.h + i, 4);
            float objectness = predictions[obj_index]; 
            if(objectness <= thresh) continue; // 실제 모델을 거쳐 나온 l.output을 통한 objectness가 임계점보다 작으면 무시
            int box_index  = entry_index(l, 0, n*l.w*l.h + i, 0); // 임계점보다 큰 경우에는 
            //해당 anchor box와 유사하기 때문에 해당 정보를 저장
            dets[count].bbox = get_yolo_box(predictions, l.biases, l.mask[n], box_index, col, row, l.w, l.h, netw, neth, l.w*l.h);
            dets[count].objectness = objectness;
            dets[count].classes = l.classes;
            for(j = 0; j < l.classes; ++j){ // 무슨 객체 점수가 가장 높은지를 확인(추후 1classes에 대한 학습 방법 고안)
                int class_index = entry_index(l, 0, n*l.w*l.h + i, 4 + 1 + j);
                float prob = objectness*predictions[class_index];
                dets[count].prob[j] = (prob > thresh) ? prob : 0; // 해당 값이 thresh값을 못 넘길 경우 0으로
            }
            ++count;
        }
    }
    correct_yolo_boxes(dets, count, w, h, netw, neth, relative);
    return count;
}

#ifdef GPU

void forward_yolo_layer_gpu(const layer l, network net)
{
    copy_gpu(l.batch*l.inputs, net.input_gpu, 1, l.output_gpu, 1);
    int b, n;
    for (b = 0; b < l.batch; ++b){
        for(n = 0; n < l.n; ++n){
            int index = entry_index(l, b, n*l.w*l.h, 0);
            activate_array_gpu(l.output_gpu + index, 2*l.w*l.h, LOGISTIC);
            index = entry_index(l, b, n*l.w*l.h, 4);
            activate_array_gpu(l.output_gpu + index, (1+l.classes)*l.w*l.h, LOGISTIC);
        }
    }
    if(!net.train || l.onlyforward){
        cuda_pull_array(l.output_gpu, l.output, l.batch*l.outputs);
        return;
    }

    cuda_pull_array(l.output_gpu, net.input, l.batch*l.inputs);
    forward_yolo_layer(l, net);
    cuda_push_array(l.delta_gpu, l.delta, l.batch*l.outputs);
}

void backward_yolo_layer_gpu(const layer l, network net)
{
    axpy_gpu(l.batch*l.inputs, 1, l.delta_gpu, 1, net.delta_gpu, 1);
}
#endif

