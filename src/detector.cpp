#include "opencv2/core/types.hpp"
#include "opencv2/imgproc.hpp"
#include "model.hpp"
#include "utils.hpp" 
#include "logger.hpp"

#include "NvInfer.h"
#include "NvOnnxParser.h"
#include <algorithm>
#include <string>

#include "opencv2/core/core.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc//imgproc.hpp"
#include "opencv2/opencv.hpp"
#include "detector.hpp"
#include "preprocess.hpp"
#include "postprocess.hpp"
#include "coco_labels.hpp"

using namespace std;
using namespace nvinfer1;

namespace model{

namespace detector {

float iou_calc(bbox bbox1, bbox bbox2){
    auto inter_x0 = std::max(bbox1.x0, bbox2.x0);
    auto inter_y0 = std::max(bbox1.y0, bbox2.y0);
    auto inter_x1 = std::min(bbox1.x1, bbox2.x1);
    auto inter_y1 = std::min(bbox1.y1, bbox2.y1);

    float inter_w = inter_x1 - inter_x0;
    float inter_h = inter_y1 - inter_y0;
    
    float inter_area = inter_w * inter_h;
    float union_area = 
        (bbox1.x1 - bbox1.x0) * (bbox1.y1 - bbox1.y0) + 
        (bbox2.x1 - bbox2.x0) * (bbox2.y1 - bbox2.y0) - 
        inter_area;
    
    return inter_area / union_area;
}


void Detector::setup(void const* data, size_t size) {
   /*
     * detector setup需要做的事情
     * 创建engine, context，设置bindings，分配memory空间
   */

    m_runtime     = shared_ptr<IRuntime>(createInferRuntime(*m_logger), destroy_trt_ptr<IRuntime>);
    m_engine      = shared_ptr<ICudaEngine>(m_runtime->deserializeCudaEngine(data, size), destroy_trt_ptr<ICudaEngine>);
    m_context     = shared_ptr<IExecutionContext>(m_engine->createExecutionContext(), destroy_trt_ptr<IExecutionContext>);
    m_inputDims   = m_context->getBindingDimensions(0);
    m_outputDims  = m_context->getBindingDimensions(1);

    CUDA_CHECK(cudaStreamCreate(&m_stream));
    
    m_inputSize     = m_params->img.h * m_params->img.w * m_params->img.c * sizeof(float);
    m_imgArea       = m_params->img.h * m_params->img.w;
    m_outputSize    = m_outputDims.d[1] * m_outputDims.d[2] * sizeof(float);

    // 这里对host和device上的memory一起分配空间
    CUDA_CHECK(cudaMallocHost(&m_inputMemory[0], m_inputSize));
    CUDA_CHECK(cudaMallocHost(&m_outputMemory[0], m_outputSize));
    CUDA_CHECK(cudaMalloc(&m_inputMemory[1], m_inputSize));
    CUDA_CHECK(cudaMalloc(&m_outputMemory[1], m_outputSize));

    // 创建m_bindings，寻址直接从这里找
    m_bindings[0] = m_inputMemory[1];
    m_bindings[1] = m_outputMemory[1];
}

void Detector::reset_task(){
    m_bboxes.clear();
}

bool Detector::preprocess_cpu() {

    /*Preprocess -- 读取数据*/
    m_inputImage = cv::imread(m_imagePath);
    if (m_inputImage.data == nullptr) {
        LOGE("ERROR: Image file not founded! Program terminated"); 
        return false;
    }

    /*Preprocess -- 测速*/
    m_timer->start_cpu();

    /*Preprocess -- resize*/
    cv::Mat m_inputImage_clone = m_inputImage.clone();
    cv::resize(m_inputImage_clone, m_inputImage_clone, 
               cv::Size(m_params->img.w, m_params->img.h), 0, 0, cv::INTER_LINEAR);

    /*Preprocess -- host端进行normalization和BGR2RGB, NHWC->NCHW*/
    int index;
    int offset_ch0 = m_imgArea * 0;
    int offset_ch1 = m_imgArea * 1;
    int offset_ch2 = m_imgArea * 2;
    for (int i = 0; i < m_inputDims.d[2]; i++) {
        for (int j = 0; j < m_inputDims.d[3]; j++) {
            index = i * m_inputDims.d[3] * m_inputDims.d[1] + j * m_inputDims.d[1];
            m_inputMemory[0][offset_ch2++] = m_inputImage_clone.data[index + 0] / 255.0f;
            m_inputMemory[0][offset_ch1++] = m_inputImage_clone.data[index + 1] / 255.0f;
            m_inputMemory[0][offset_ch0++] = m_inputImage_clone.data[index + 2] / 255.0f;
        }
    }

    /*Preprocess -- 将host的数据移动到device上*/
    CUDA_CHECK(cudaMemcpyAsync(m_inputMemory[1], m_inputMemory[0], m_inputSize, cudaMemcpyKind::cudaMemcpyHostToDevice, m_stream));

    m_timer->stop_cpu();
    m_timer->duration_cpu<timer::Timer::ms>("preprocess(CPU)");
    return true;
}

bool Detector::preprocess_gpu() {

    /*Preprocess -- 读取数据*/
    m_inputImage = cv::imread(m_imagePath);
    if (m_inputImage.data == nullptr) {
        LOGE("ERROR: file not founded! Program terminated"); return false;
    }
    
    /*Preprocess -- 测速*/
    m_timer->start_gpu();

    /*Preprocess -- 使用GPU进行warpAffine, 并将结果返回到m_inputMemory中*/
    preprocess::preprocess_resize_gpu(m_inputImage, m_inputMemory[1],
                                   m_params->img.h, m_params->img.w, 
                                   preprocess::tactics::GPU_WARP_AFFINE);

    m_timer->stop_gpu();
    m_timer->duration_gpu("preprocess(GPU)");
    return true;
}


bool Detector::postprocess_cpu() {
    m_timer->start_cpu();

    /*Postprocess -- 将device上的数据移动到host上*/
    int output_size = m_outputDims.d[1] * m_outputDims.d[2] * sizeof(float);
    CUDA_CHECK(cudaMemcpyAsync(m_outputMemory[0], m_outputMemory[1], output_size, cudaMemcpyKind::cudaMemcpyDeviceToHost, m_stream));
    CUDA_CHECK(cudaStreamSynchronize(m_stream));

    /*Postprocess -- yolov5后处理*/
    /*
     * 1. 模型输出数据进行decode，把获取的bbox放入到m_bboxes中
     * 2. decode得到的m_bboxes根据nms threshold进行NMS处理
     * 3. 把最终得到的bbox绘制到原图中
     */

    float conf_threshold = 0.3; 
    float nms_threshold  = 0.5;

    /*Postprocess -- 1. decode*/
    /*
     * 输出[batch, bboxes, ch] 1x9072x13 转换为vector<bbox>
     * 转换好的x0, y0, x1, y1，以及confidence和classness会存入到box中，并push到m_bboxes中，准备接下来的NMS处理
     */
    int    boxes_count = m_outputDims.d[1];
    int    class_count = m_outputDims.d[2] - 5;
    float* tensor;

    float  cx, cy, w, h, obj, prob, conf,obj_conf;
    float  x0, y0, x1, y1;
    int    label;

    for (int i = 0; i < boxes_count; i ++){
        tensor = m_outputMemory[0] + i * m_outputDims.d[2];
        
        obj_conf   = tensor[4];
        if (obj_conf < conf_threshold)
            continue;
        
        label  = max_element(tensor + 5, tensor + 5 + class_count) - (tensor + 5);
        conf   = tensor[5 + label]*obj_conf;
        if (conf < conf_threshold)
            continue;
        cx = tensor[0];
        cy = tensor[1];
        w  = tensor[2];
        h  = tensor[3];
        
        x0 = cx - w / 2;
        y0 = cy - h / 2;
        x1 = x0 + w;
        y1 = y0 + h;

        // 仿射变换：512x288——>1280x720
        preprocess::warpaffine_init(720, 1280, 288, 512);
        preprocess::affine_transformation(preprocess::affine_matrix.reverse, x0, y0, &x0, &y0);
        preprocess::affine_transformation(preprocess::affine_matrix.reverse, x1, y1, &x1, &y1);
        bbox yolo_box(x0, y0, x1, y1, conf, label);
        m_bboxes.emplace_back(yolo_box);
    }
    LOGD("the count of decoded bbox is %d", m_bboxes.size());
    

    /*Postprocess -- 2. NMS*/

    vector<bbox> final_bboxes;
    final_bboxes.reserve(m_bboxes.size());
    std::sort(m_bboxes.begin(), m_bboxes.end(), 
              [](bbox& box1, bbox& box2){return box1.confidence > box2.confidence;});

    /*
     * 频繁的对vector的大小的更改的空间复杂度会比较大
     * 通过给bbox设置skip计算的flg来调整。
    */
    for(int i = 0; i < m_bboxes.size(); i ++){
        if (m_bboxes[i].flg_remove)
            continue;
        
        final_bboxes.emplace_back(m_bboxes[i]);
        for (int j = i + 1; j < m_bboxes.size(); j ++) {
            if (m_bboxes[j].flg_remove)
                continue;

            if (m_bboxes[i].label == m_bboxes[j].label){
                if (iou_calc(m_bboxes[i], m_bboxes[j]) > nms_threshold)
                    m_bboxes[j].flg_remove = true;
            }
        }
    }

    LOGD("the count of bbox after NMS is %d", final_bboxes.size());

    /*Postprocess -- draw_bbox*/
    string tag   = "detect-" + getPrec(m_params->prec);
    m_outputPath = changePath(m_imagePath, "../result", ".png", tag);

    int   font_face  = 0;
    float font_scale = 0.001 * MIN(m_inputImage.cols, m_inputImage.rows);
    int   font_thick = 2;
    int   baseline;
    CocoLabels labels;
    
    LOG("\tResult:");
    for (int i = 0; i < final_bboxes.size(); i ++){
        auto box = final_bboxes[i];
        auto name = labels.coco_get_label(box.label);
        auto rec_color = labels.coco_get_color(box.label);
        auto txt_color = labels.get_inverse_color(rec_color);
        auto txt = cv::format({"%s: %.2f%%"}, name.c_str(), box.confidence * 100);
        auto txt_size = cv::getTextSize(txt, font_face, font_scale, font_thick, &baseline);

        int txt_height = txt_size.height + baseline + 10;
        int txt_width  = txt_size.width + 3;
        
        cv::Point txt_pos(round(box.x0), round(box.y0 - (txt_size.height - baseline + font_thick)));
        cv::Rect  txt_rec(round(box.x0 - font_thick), round(box.y0 - txt_height), txt_width, txt_height);
        cv::Rect  box_rec(round(box.x0), round(box.y0), round(box.x1 - box.x0), round(box.y1 - box.y0));

        cv::rectangle(m_inputImage, box_rec, rec_color, 3);
        cv::rectangle(m_inputImage, txt_rec, rec_color, -1);
        cv::putText(m_inputImage, txt, txt_pos, font_face, font_scale, txt_color, font_thick, 16);

    }
    LOG("\tSummary:");
    LOG("\t\tDetected Objects: %d", final_bboxes.size());
    LOG("");

    m_timer->stop_cpu();
    m_timer->duration_cpu<timer::Timer::ms>("postprocess(CPU)");

    cv::imwrite(m_outputPath, m_inputImage);
    LOG("\tsave image to %s\n", m_outputPath.c_str());

    return true;
}

//用于对比测试后处理在CPU和GPU上的速度
// bool Detector::postprocess_gpu() {
//     postprocess_cpu();
// }

bool Detector::postprocess_gpu() {
    m_timer->start_gpu();
    
    /*Postprocess -- 当前模型输出数据在device上，无需移动到host上*/
    
    float conf_threshold = 0.3; 
    float nms_threshold  = 0.5; 
    int    boxes_count = m_outputDims.d[1];
    int    class_count = m_outputDims.d[2] - 5;
    

    /*Postprocess -- 1. decode核函数 和 2. NMS核函数 在decode_bbox_gpu中按顺序执行*/
    
    std::vector<bbox> final_bboxes = postprocess::decode_bbox_gpu(m_outputMemory[1], boxes_count, class_count);
    m_timer->stop_gpu();
    m_timer->duration_gpu("postprocess(GPU)");

    /*Postprocess -- draw_bbox*/
    string tag   = "detect-" + getPrec(m_params->prec);
    m_outputPath = changePath(m_imagePath, "../result", ".png", tag);

    int   font_face  = 0;
    float font_scale = 0.001 * MIN(m_inputImage.cols, m_inputImage.rows);
    int   font_thick = 2;
    int   baseline;
    CocoLabels labels;

    LOG("\tResult:");
    for (int i = 0; i < final_bboxes.size(); i ++){
        auto box = final_bboxes[i];
        auto name = labels.coco_get_label(box.label);
        auto rec_color = labels.coco_get_color(box.label);
        auto txt_color = labels.get_inverse_color(rec_color);
        auto txt = cv::format({"%s: %.2f%%"}, name.c_str(), box.confidence * 100);
        auto txt_size = cv::getTextSize(txt, font_face, font_scale, font_thick, &baseline);

        int txt_height = txt_size.height + baseline + 10;
        int txt_width  = txt_size.width + 3;

        cv::Point txt_pos(round(box.x0), round(box.y0 - (txt_size.height - baseline + font_thick)));
        cv::Rect  txt_rec(round(box.x0 - font_thick), round(box.y0 - txt_height), txt_width, txt_height);
        cv::Rect  box_rec(round(box.x0), round(box.y0), round(box.x1 - box.x0), round(box.y1 - box.y0));

        cv::rectangle(m_inputImage, box_rec, rec_color, 3);
        cv::rectangle(m_inputImage, txt_rec, rec_color, -1);
        cv::putText(m_inputImage, txt, txt_pos, font_face, font_scale, txt_color, font_thick, 16);

    }
    LOG("\tSummary:");
    LOG("\t\tDetected Objects: %d", final_bboxes.size());
    LOG("");

    

    cv::imwrite(m_outputPath, m_inputImage);
    LOG("\tsave image to %s\n", m_outputPath.c_str());

    return true;
}


shared_ptr<Detector> make_detector(
    std::string onnx_path, logger::Level level, Params params)
{
    return make_shared<Detector>(onnx_path, level, params);
}

}; // namespace detector
}; // namespace model
