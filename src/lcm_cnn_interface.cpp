#include "lcm_cnn_interface.hpp" 

Handler::~Handler() {}

void Handler::receiveLegControlMsg(const lcm::ReceiveBuffer* rbuf,
                                const std::string& chan, 
                                const leg_control_data_lcmt* msg)
{

    int size = 12;
    float* leg_control_data = new float[48]();
    arrayCopy(leg_control_data, msg->q, size);
    arrayCopy(leg_control_data + size, msg->qd, size);
    arrayCopy(leg_control_data + size + size, msg->p, size);
    arrayCopy(leg_control_data + size + size + size, msg->v, size);
	
    /// LOW: 500Hz version:
    cnnInputLegQueue.push(leg_control_data);

    /// HIGH: 1000Hz version:
    // If the latest_idx reaches the limit of the buffer vector:
    // if (latest_idx == cnn_input_leg_vector.size() - 1) {
    //     latest_idx = -1;
    // }
    // if (cnn_input_leg_vector[latest_idx + 1] != nullptr) {
    //     delete[] cnn_input_leg_vector[latest_idx + 1];
    // }
    // cnn_input_leg_vector[++latest_idx] = leg_control_data;
}

void Handler::receiveMicrostrainMsg(const lcm::ReceiveBuffer* rbuf,
                                const std::string& chan, 
                                const microstrain_lcmt* msg)
{
    /// LOW: 500Hz version:
    if (cnnInputLegQueue.size() >= cnnInputIMUQueue.size()) {
        float* microstrain_data = new float[6]();
        int size = 3;
        arrayCopy(microstrain_data, msg->acc, size);
        arrayCopy(microstrain_data + size, msg->omega, size);

        cnnInputIMUQueue.push(microstrain_data);
    }

    /// HIGH: 1000Hz version:
    // if (latest_idx != -1 && cnn_input_leg_vector[latest_idx] != nullptr) {
    //     float* microstrain_data = new float[6]();
    //     int size = 3;
    //     arrayCopy(microstrain_data, msg->acc, size);
    //     arrayCopy(microstrain_data + size, msg->omega, size);

    //     cnnInputIMUQueue.push(microstrain_data);
    //     float* leg_control_data = new float[48]();
    //     arrayCopy(leg_control_data, cnn_input_leg_vector[latest_idx], 48);
    //     cnnInputLegQueue.push(leg_control_data);
    // }
}

void Handler::receiveContactGroundTruthMsg(const lcm::ReceiveBuffer* rbuf,
                                        const std::string& chan, 
                                        const contact_ground_truth_t* msg)
{
    std::vector<int8_t> contact_ground_truth_label = msg->contact;
    
    int gt_label = contact_ground_truth_label[0] * 2 * 2 * 2 
                + contact_ground_truth_label[1] * 2 * 2
                + contact_ground_truth_label[2] * 2
                + contact_ground_truth_label[3];

    cnnInputGtLabelQueue.push(gt_label);
}


void Handler::arrayCopy(float array1 [], const float array2 [], int size) {
    for (int i = 0; i < size; ++i) {
        array1[i] = array2[i];
    }
}


const std::string gSampleName = "TensorRT.sample_onnx";


TensorRTAccelerator::TensorRTAccelerator(const samplesCommon::OnnxSampleParams& params)
    : mParams(params),
      mEngine(nullptr)
{
}

TensorRTAccelerator::~TensorRTAccelerator() {};


bool TensorRTAccelerator::buildFromSerializedEngine()
{
    /// REMARK: we can deserialize a serialized engine if we have one:
    // -----------------------------------------------------------------------------------------------------------------------
    nvinfer1::IRuntime* runtime = nvinfer1::createInferRuntime(sample::gLogger);
    std::string cached_path = PROGRAM_PATH + "engines/0730_2blocks_best_val_loss.trt";
    std::ifstream fin(cached_path);
    std::string cached_engine = "";
    while (fin.peek() != EOF) {
        std::stringstream buffer;
        buffer << fin.rdbuf();
        cached_engine.append(buffer.str());
    }
    fin.close();
    mEngine = std::shared_ptr<nvinfer1::ICudaEngine> (
                                    runtime->deserializeCudaEngine(cached_engine.data(), cached_engine.size(), nullptr),
                                    samplesCommon::InferDeleter());
    if (!mEngine)
    {
        return false;
    }
    
    context = SampleUniquePtr<nvinfer1::IExecutionContext>(mEngine->createExecutionContext());
    if (!context)
    {
        return false;
    }
    
    std::cout << "Successfully built the engine and made the context" << std::endl;

    return true;
}


int TensorRTAccelerator::infer(float* cnn_input_matrix_normalized)
{
    // Create RAII buffer manager object

    if (!mEngine) {
        std::cerr << "Failed to load mEngine" << std::endl;
        return false;
    }
    
    samplesCommon::BufferManager buffers(mEngine);

    // Read the input data into the managed buffers


    assert(mParams.inputTensorNames.size() == 1);
    if (!processInput(buffers, cnn_input_matrix_normalized))
    {
        std::cerr << "Failed in reading input" << std::endl;
        return false;
    }


    // Memcpy from host input buffers to device input buffers
    buffers.copyInputToDevice();

    bool status = context->executeV2(buffers.getDeviceBindings().data());
    if (!status)
    {
        std::cerr << "Failed in making execution" << std::endl;
        return false;
    }
    
    // Memcpy from device output buffers to host output buffers
    buffers.copyOutputToHost();

    // Get results from the engine and return the output
    return getOutput(buffers);
}

bool TensorRTAccelerator::serialize() {
    nvinfer1::IHostMemory *serializedModel = mEngine->serialize();
    std::string serialize_str;
    std::ofstream serialize_output_stream;
    serialize_str.resize(serializedModel->size());
    memcpy((void*)serialize_str.data(), serializedModel->data(), serializedModel->size());
    serialize_output_stream.open(PROGRAM_PATH + "engines/0730_2blocks_best_val_loss.trt");
    
    serialize_output_stream << serialize_str;
    serialize_output_stream.close();
    serializedModel->destroy();
    
    std::cout << "Successfully serialized the engine" << std::endl;
    return true;
}

//!
//! \brief Reads the input and stores the result in a managed buffer
//!
bool TensorRTAccelerator::processInput(const samplesCommon::BufferManager& buffers, const float* cnn_input_matrix_normalized)
{   
    const int inputH = 75;
    const int inputW = 54;
    
    float* hostDataBuffer = static_cast<float*>(buffers.getHostBuffer(mParams.inputTensorNames[0]));
    int number_of_items = 75 * 54;
    // hostDataBuffer.resize(number_of_items);
    for (int i = 0; i < inputH * inputW; i++) {
        // std::cout <<  cnn_input_matrix_normalized[i] << std::endl;
        hostDataBuffer[i] = cnn_input_matrix_normalized[i];
    }

    return true;
}


int TensorRTAccelerator::getOutput(const samplesCommon::BufferManager& buffers)
{
    const int outputSize = 16; // 4 legs, 16 status
    
    float* output = static_cast<float*>(buffers.getHostBuffer(mParams.outputTensorNames[0]));
    float val{0.0f};
    int idx{0};

    float current_max = output[0];
    int output_idx = 0;
    for (int i = 1; i < outputSize; i++) {
        // sample::gLogInfo << "Leg status " << i << " is " << output[i] << std::endl;
        if (output[i] > current_max) {
            current_max = output[i];
            output_idx = i;
        }
    }
    return output_idx;
}


//!
//! \brief Initializes members of the params struct using the command line args
//!
samplesCommon::OnnxSampleParams initializeSampleParams(const samplesCommon::Args& args)
{
    samplesCommon::OnnxSampleParams params;
    if (args.dataDirs.empty()) //!< Use default directories if user hasn't provided directory paths
    {
        std::cout << "Using default directory" << endl;
        params.dataDirs.push_back("weights/");
        params.dataDirs.push_back("data/");
    }
    else //!< Use the data directory provided by the user
    {
        std::cout << "Using directory provided by the user" << endl;
        params.dataDirs = args.dataDirs;
    }
    params.onnxFileName = "0730_2blocks_best_val_loss.onnx";
    params.inputTensorNames.push_back("input");
    params.batchSize = 1; //!< Takes in 1 batch every time
    params.outputTensorNames.push_back("output");
    params.dlaCore = args.useDLACore;
    params.int8 = args.runInInt8;
    params.fp16 = args.runInFp16;

    return params;
}

//!
//! \brief Prints the help information for running this sample
//!
void printHelpInfo()
{
    std::cout
        << "Usage: ./sample_onnx_mnist [-h or --help] [-d or --datadir=<path to data directory>] [--useDLACore=<int>]"
        << std::endl;
    std::cout << "--help          Display help information" << std::endl;
    std::cout << "--datadir       Specify path to a data directory, overriding the default. This option can be used "
                 "multiple times to add multiple directories. If no data directories are given, the default is to use "
                 "(data/samples/mnist/, data/mnist/)"
              << std::endl;
    std::cout << "--useDLACore=N  Specify a DLA engine for layers that support DLA. Value can range from 0 to n-1, "
                 "where n is the number of DLA engines on the platform."
              << std::endl;
    std::cout << "--int8          Run in Int8 mode." << std::endl;
    std::cout << "--fp16          Run in FP16 mode." << std::endl;
}


LcmCnnInterface::LcmCnnInterface(const samplesCommon::Args &args)
    : input_h(75),
      input_w(54),
      data_require(75),
      new_line(input_w, 0),
      sum_of_rows(input_w, 0),
      sum_of_rows_square(input_w, 0),
      previous_first_row(input_w, 0),
      cnn_input_matrix(input_h, std::vector<float>(input_w)),
      mean_vector(input_w, 0),
      std_vector(input_w, 0),
      is_first_full_matrix(true)
{}

LcmCnnInterface::~LcmCnnInterface() {
    while (!cnnInputLegQueue.empty()) {
        delete[] cnnInputLegQueue.front();
        cnnInputLegQueue.pop();
    }

    while (!cnnInputIMUQueue.empty()) {
        delete[] cnnInputIMUQueue.front();
        cnnInputIMUQueue.pop();
    }
}

void LcmCnnInterface::buildMatrix (std::queue<float *>& cnn_input_queue, std::queue<float *>& new_data_queue){
    // Get leg input from queue
    while (true){
        if (!cnnInputLegQueue.empty() && !cnnInputIMUQueue.empty()){
            // mtx.lock();
            // // Get GTlabel from queue
            // int gtLabel = cnnInputGtLabelQueue.front();
            // cnnInputGtLabelQueue.pop();
            // mtx.unlock();
            // Start to build a new line and generate a new input
            int idx = 0; //!< keep track of the current new_line idx;
            int legTypeDataNum = 12;
            int IMUTypeDataNum = 3;
            float* new_data = new float[input_w]();

            // get input data:
            mtx.lock();
            for (int i = 0; i < legTypeDataNum; ++i){
                new_line[idx] = cnnInputLegQueue.front()[i];
                new_data[idx] = new_line[idx];
                ++idx;
            }
            // leg_control_data.qd:
            for (int i = 0; i < legTypeDataNum; ++i) {
                new_line[idx] = cnnInputLegQueue.front()[i + legTypeDataNum];
                new_data[idx] = new_line[idx];
                ++idx;
            }
            // microstrain(IMU).acc:
            for (int i = 0; i < IMUTypeDataNum; ++i) {
                new_line[idx] = cnnInputIMUQueue.front()[i];
                new_data[idx] = new_line[idx];
                ++idx;
            }
            // microstrain(IMU).omega:
            for (int i = 0; i < IMUTypeDataNum; ++i) {
                new_line[idx] = cnnInputIMUQueue.front()[i + IMUTypeDataNum];
                new_data[idx] = new_line[idx];
                ++idx;
            }
            // leg_control_data.p:
            for (int i = 0; i < legTypeDataNum; ++i) {
                new_line[idx] = cnnInputLegQueue.front()[i + legTypeDataNum + legTypeDataNum];
                new_data[idx] = new_line[idx];
                ++idx;
            }

            // leg_control_data.v:
            for (int i = 0; i < legTypeDataNum; ++i) {
                new_line[idx] = cnnInputLegQueue.front()[i + legTypeDataNum + legTypeDataNum + legTypeDataNum];
                new_data[idx] = new_line[idx];
                ++idx;
            }
            
            // release memory:
            delete[] cnnInputLegQueue.front();
            delete[] cnnInputIMUQueue.front();
            cnnInputLegQueue.pop();
            cnnInputIMUQueue.pop();
            mtx.unlock();

            new_data_queue.push(new_data);

            // Put the new_line to the InputMatrix and destroy the first line:
            cnn_input_matrix.erase(cnn_input_matrix.begin());
            cnn_input_matrix.push_back(new_line);
            data_require = std::max(data_require - 1, 0);
            
            
            if (data_require == 0) {            
                normalizeAndInfer(cnn_input_queue);   
                // std::cout << "The ground truth label is: " << gtLabel << std::endl;
                break;
            }
        } 
    }
}


void LcmCnnInterface::normalizeAndInfer(std::queue<float *>& cnn_input_queue) {
    /// REMARK: normalize input and send to CNN network in TRT
    // We need to normalize the input matrix, to do so,
    // we need to calculate the mean value and standard deviation.
    if (is_first_full_matrix) {
        runFullCalculation(cnn_input_queue);
        is_first_full_matrix = false;
    }
    else {
        runSlidingWindow(cnn_input_queue);
    }    
}


void LcmCnnInterface::runFullCalculation(std::queue<float *>& cnn_input_queue) {
    float* cnn_input_matrix_normalized = new float[input_h * input_w]();
    
    std::ofstream data_file;
    data_file.open("input_matrix_500Hz.bin", ios::out | ios::binary);
    if (!data_file) {
        std::cerr << " Cannot open the file!" << std::endl;
        return -1;
    }
    
    for (int j = 0; j < input_w; ++j) {
        // find mean:
        for (int i = 0; i < input_h; ++i) {
            sum_of_rows[j] += cnn_input_matrix[i][j];
            sum_of_rows_square[j] += std::pow(cnn_input_matrix[i][j], 2.0);
        }
        mean_vector[j] = sum_of_rows[j] / input_h;

        // find std:
        for (int i = 0; i < input_h; ++i) {
            std_vector[j] += std::pow((cnn_input_matrix[i][j] - mean_vector[j]), 2.0);
        }
        std_vector[j] = std::sqrt(std_vector[j] / (input_h - 1));
     
        // Normalize the matrix:
        for (int i = 0; i < input_h; ++i) {
            cnn_input_matrix_normalized[i * input_w + j] = (cnn_input_matrix[i][j] - mean_vector[j]) / std_vector[j];
            /// REMARK: delete the following lines in actual use:
            data_file.write((char *) &cnn_input_matrix_normalized[i * input_w + j], sizeof(float));
            data_file.close();
        }
        previous_first_row[j] = cnn_input_matrix[0][j];
    }
    
    cnn_input_queue.push(cnn_input_matrix_normalized);
}

void LcmCnnInterface::runSlidingWindow(std::queue<float *>& cnn_input_queue) {
    float* cnn_input_matrix_normalized = new float[input_h * input_w]();

    for (int j = 0; j < input_w; ++j) {
        // find mean:
        sum_of_rows[j] = sum_of_rows[j] - previous_first_row[j] + new_line[j];
        sum_of_rows_square[j] = sum_of_rows_square[j] - std::pow(previous_first_row[j], 2.0) + std::pow(new_line[j], 2.0);

        mean_vector[j] = sum_of_rows[j] / input_h;

        // find std:
        std_vector[j] = sum_of_rows_square[j] - 2 * mean_vector[j] * sum_of_rows[j] + input_h * std::pow(mean_vector[j], 2.0);
        std_vector[j] = std::sqrt(std_vector[j] / (input_h - 1));
     
        // Normalize the matrix:
        for (int i = 0; i < input_h; ++i) {
            cnn_input_matrix_normalized[i * input_w + j] = (cnn_input_matrix[i][j] - mean_vector[j]) / std_vector[j];
            
        }
        previous_first_row[j] = cnn_input_matrix[0][j];
    }

    cnn_input_queue.push(cnn_input_matrix_normalized);
}

ContactEstimation::ContactEstimation(const samplesCommon::Args &args)
    : input_h(75),
      input_w(54),
      sample(initializeSampleParams(args))
{
    // cnn_input_matrix_normalized = new float[input_h * input_w];
    if (!sample.buildFromSerializedEngine()) {
        std::cerr << "FAILED: Cannot build the engine" << std::endl;
        return;
    }
    if (!lcm.good())
	    return;
    cnn_output.num_legs = 4;
    cnn_output.contact = {0, 0, 0, 0};
}

ContactEstimation::~ContactEstimation() {}

void ContactEstimation::makeInference(std::queue<float *>& cnn_input_queue, std::queue<float *>& new_data_queue) {
    while (true) {
        if (!cnn_input_queue.empty()) {
            /// REMARK: Inference (Here we added a timer to calculate the inference frequency)
            cudaEvent_t start, stop;
            cudaEventCreate(&start);
            cudaEventCreate(&stop);
            cudaEventRecord(start);

            mtx.lock();
            float* cnn_input_matrix_normalized = cnn_input_queue.front();
            cnn_input_queue.pop();
            float* new_data = new_data_queue.front();
            new_data_queue.pop();
            mtx.unlock();

	    int idx = 0;
	    const int legTypeDataNum = 12;
	    const int IMUTypeDataNum = 3;
	    for (int i = 0; i < legTypeDataNum; ++i){
                cnn_output.q[i] = new_data[idx];
                ++idx;
            }
            // leg_control_data.qd:
            for (int i = 0; i < legTypeDataNum; ++i) {
                cnn_output.qd[i] = new_data[idx];
                ++idx;
            }
            // microstrain(IMU).acc:
            for (int i = 0; i < IMUTypeDataNum; ++i) {
                cnn_output.acc[i] = new_data[idx];
                ++idx;
            }
            // microstrain(IMU).omega:
            for (int i = 0; i < IMUTypeDataNum; ++i) {
                cnn_output.omega[i] = new_data[idx];
                ++idx;
            }
            // leg_control_data.p:
            for (int i = 0; i < legTypeDataNum; ++i) {
                cnn_output.p[i] = new_data[idx];
                myfile_leg_p << new_data[idx] << ',';
                ++idx;
            }
            myfile_leg_p << '\n';
    	    myfile_leg_p.flush();

            // leg_control_data.v:
            for (int i = 0; i < legTypeDataNum; ++i) {
                cnn_output.v[idx] = new_data[idx];
                ++idx;
            }
			
            int output_idx = sample.infer(cnn_input_matrix_normalized);
            if (output_idx == -1)
            {
                std::cerr << "FAILED: Cannot use the engine to infer a result" << std::endl;
                return;
            }
            delete[] cnn_input_matrix_normalized;
            delete[] new_data;
            publishOutput(output_idx);

            cudaEventRecord(stop);
            cudaEventSynchronize(stop);
            float milliseconds = 0;
            cudaEventElapsedTime(&milliseconds, start, stop);
            // std::cout << "It's frequency is " << 1000 / milliseconds << " Hz" << std::endl;
        }
    }
}


void ContactEstimation::publishOutput(int output_idx) {
    std::string binary = std::bitset<4>(output_idx).to_string(); // to binary
    for (int i = 0; i < cnn_output.num_legs; i++) {
        cnn_output.contact[i] = binary[i];
        myfile << cnn_output.contact[i] << ',';
    }
    myfile << '\n';
    myfile << std::flush;
    lcm.publish("synced_proprioceptive_data", &cnn_output);
}



int main(int argc, char** argv)
{
    /// LCM: subscribe to channels:
    lcm::LCM lcm;
    if(!lcm.good())
        return 1;
    Handler handlerObject;
    lcm.subscribe("leg_control_data", &Handler::receiveLegControlMsg, &handlerObject);
    lcm.subscribe("microstrain", &Handler::receiveMicrostrainMsg, &handlerObject);
    // lcm.subscribe("contact_ground_truth", &Handler::receiveContactGroundTruthMsg, &handlerObject);
    
    std::cout << "Start Running LCM-CNN Interface" << std::endl;
    
    // Takes input arguments
    samplesCommon::Args args;
    bool argsOK = samplesCommon::parseArgs(args, argc, argv);
    if (!argsOK)
    {
        sample::gLogError << "Invalid arguments" << std::endl;
        printHelpInfo();
        return -1;
    }
    if (args.help)
    {
        printHelpInfo();
        return -1;
    }

    /// INTERFACE: use multiple threads to avoid missing messages:
    std::queue<float *> cnn_input_queue;
    std::queue<float *> new_data_queue;
    myfile.open(PROGRAM_PATH + "contact_est_lcm.csv");
	myfile_leg_p.open(PROGRAM_PATH + "p_lcm.csv");
    LcmCnnInterface matrix_builder(args);
	ContactEstimation engine_builder(args);
    std::thread BuildMatrixThread (&LcmCnnInterface::buildMatrix, &matrix_builder, std::ref(cnn_input_queue), std::ref(new_data_queue));
    std::thread CNNInferenceThread (&ContactEstimation::makeInference, &engine_builder, std::ref(cnn_input_queue), std::ref(new_data_queue));

    while(0 == lcm.handle());
    BuildMatrixThread.join();
    CNNInferenceThread.join();

    myfile.close();
    myfile_leg_p.close();


    return 0;
}
