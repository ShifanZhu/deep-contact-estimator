#include "include/src/lcm_cnn_interface.hpp"

int main(int argc, char **argv)
{
    /// LCM: subscribe to channels:
    lcm::LCM lcm;
    if (!lcm.good())
        return 1;
    
    LcmMsgQueues_t lcm_msg_in;
    std::mutex mtx;
    LcmHandler handlerObject(&lcm, &lcm_msg_in, &mtx);
    // lcm.subscribe("leg_control_data", &Handler::receiveLegControlMsg, &handlerObject);
    // lcm.subscribe("microstrain", &Handler::receiveMicrostrainMsg, &handlerObject);
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

    if (debug_flag == 1)
    {
        myfile.open(PROGRAM_PATH + "contact_est_lcm.csv");
        myfile_leg_p.open(PROGRAM_PATH + "p_lcm.csv");
    }

    LcmCnnInterface matrix_builder(args, &lcm_msg_in, &mtx);
    ContactEstimation engine_builder(args, &lcm);
    std::thread BuildMatrixThread(&LcmCnnInterface::buildMatrix, &matrix_builder, std::ref(cnn_input_queue), std::ref(new_data_queue));
    std::thread CNNInferenceThread(&ContactEstimation::makeInference, &engine_builder, std::ref(cnn_input_queue), std::ref(new_data_queue));

    while (0 == lcm.handle());:LCM lcm;
    
    BuildMatrixThread.join();
    CNNInferenceThread.join();

    // myfile.close();
    // myfile_leg_p.close();

    return 0;
}

LcmCnnInterface::LcmCnnInterface(const samplesCommon::Args &args, LcmMsgQueues_t* lcm_msg_in, std::mutex* mtx)
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
      is_first_full_matrix(true),
      lcm_msg_in_(lcm_msg_in),
      mtx_(cdata_mtx)
{
}

LcmCnnInterface::~LcmCnnInterface()
{
    while (!cnn_input_leg_queue.empty())
    {
        delete[] cnn_input_leg_queue.front();
        cnn_input_leg_queue.pop();
    }

    while (!cnn_input_imu_queue.empty())
    {
        delete[] cnn_input_imu_queue.front();
        cnn_input_imu_queue.pop();
    }
}

void LcmCnnInterface::buildMatrix(std::queue<float *> &cnn_input_queue, std::queue<float *> &new_data_queue)
{
    // Get leg input from queue
    while (true)
    {
        if (!lcm_msg_in_->cnn_input_leg_queue.empty() && !lcm_msg_in_->cnn_input_imu_queue.empty())
        {
            // mtx.lock();
            // // Get GTlabel from queue
            // int gtLabel = cnn_input_gtlabel_queue.front();
            // cnn_input_gtlabel_queue.pop();
            // mtx.unlock();
            // Start to build a new line and generate a new input
            int idx = 0; //!< keep track of the current new_line idx;
            int legTypeDataNum = 12;
            int IMUTypeDataNum = 3;
            // new_data stores the pointer to the new line
            float *new_data = new float[input_w]();

            // get input data:
            mtx.lock();
            std::shared_ptr<LcmLegStruct> leg_control_data = lcm_msg_in_->cnn_inpu_leg_queue.front();
            std::shared_ptr<LcmIMUStruct> microstrain_data = lcm_msg_in_->cnn_input_imu_queue.front();
            lcm_msg_in->cnn_input_leg_queue.pop();
            lcm_msg_in->cnn_input_imu_queue.pop();
            mtx.unlock();

            // leg_control_data.q
            for (int i = 0; i < legTypeDataNum; ++i)
            {
                new_line[idx] = leg_control_data->q[i];
                new_data[idx] = new_line[idx];
                ++idx;
            }
            // leg_control_data.qd:
            for (int i = 0; i < legTypeDataNum; ++i)
            {
                new_line[idx] = leg_control_data->qd[i];
                new_data[idx] = new_line[idx];
                ++idx;
            }
            // microstrain(IMU).acc:
            for (int i = 0; i < IMUTypeDataNum; ++i)
            {
                new_line[idx] = microstrain_data->acc[i];
                new_data[idx] = new_line[idx];
                ++idx;
            }
            // microstrain(IMU).omega:
            for (int i = 0; i < IMUTypeDataNum; ++i)
            {
                new_line[idx] = microstrain_data->omega[i];
                new_data[idx] = new_line[idx];
                ++idx;
            }
            // leg_control_data.p:
            for (int i = 0; i < legTypeDataNum; ++i)
            {
                new_line[idx] = leg_control_data->p[i];
                new_data[idx] = new_line[idx];
                ++idx;
            }

            // leg_control_data.v:
            for (int i = 0; i < legTypeDataNum; ++i)
            {
                new_line[idx] = leg_control_data->v[i];
                new_data[idx] = new_line[idx];
                ++idx;
            }

            // release memory:
            new_data_queue.push(new_data);

            // Put the new_line to the InputMatrix and destroy the first line:
            cnn_input_matrix.erase(cnn_input_matrix.begin());
            cnn_input_matrix.push_back(new_line);
            data_require = std::max(data_require - 1, 0);

            if (data_require != 0)
            {
                new_data_queue.pop();
            }
            if (data_require == 0)
            {
                normalizeAndInfer(cnn_input_queue);
                // new_data_queue.push(new_data);
                // std::cout << "The ground truth label is: " << gtLabel << std::endl;
                // break;
            }
        }
    }
}

void LcmCnnInterface::normalizeAndInfer(std::queue<float *> &cnn_input_queue)
{
    /// REMARK: normalize input and send to CNN network in TRT
    // We need to normalize the input matrix, to do so,
    // we need to calculate the mean value and standard deviation.
    if (is_first_full_matrix)
    {
        runFullCalculation(cnn_input_queue);
        is_first_full_matrix = false;
    }
    else
    {
        runSlidingWindow(cnn_input_queue);
    }
}

void LcmCnnInterface::runFullCalculation(std::queue<float *> &cnn_input_queue)
{
    float *cnn_input_matrix_normalized = new float[input_h * input_w]();

    // std::ofstream data_file;
    // data_file.open("input_matrix_500Hz.bin", ios::out | ios::binary);
    // if (!data_file)
    // {
    //     std::cerr << " Cannot open the file!" << std::endl;
    //     return;
    // }

    for (int j = 0; j < input_w; ++j)
    {
        // find mean:
        for (int i = 0; i < input_h; ++i)
        {
            sum_of_rows[j] += cnn_input_matrix[i][j];
            sum_of_rows_square[j] += std::pow(cnn_input_matrix[i][j], 2.0);
        }
        mean_vector[j] = sum_of_rows[j] / input_h;

        // find std:
        for (int i = 0; i < input_h; ++i)
        {
            std_vector[j] += std::pow((cnn_input_matrix[i][j] - mean_vector[j]), 2.0);
        }
        std_vector[j] = std::sqrt(std_vector[j] / (input_h - 1));

        // Normalize the matrix:
        for (int i = 0; i < input_h; ++i)
        {
            cnn_input_matrix_normalized[i * input_w + j] = (cnn_input_matrix[i][j] - mean_vector[j]) / std_vector[j];
        }
        previous_first_row[j] = cnn_input_matrix[0][j];
    }

    /// REMARK: delete the following lines in actual use:
    // data_file.write(reinterpret_cast<char *>(&cnn_input_matrix_normalized[0]), 75 * 54 * sizeof(float));
    // data_file.close();
    cnn_input_queue.push(cnn_input_matrix_normalized);
}

void LcmCnnInterface::runSlidingWindow(std::queue<float *> &cnn_input_queue)
{
    float *cnn_input_matrix_normalized = new float[input_h * input_w]();

    for (int j = 0; j < input_w; ++j)
    {
        // find mean:
        sum_of_rows[j] = sum_of_rows[j] - previous_first_row[j] + new_line[j];
        sum_of_rows_square[j] = sum_of_rows_square[j] - std::pow(previous_first_row[j], 2.0) + std::pow(new_line[j], 2.0);

        mean_vector[j] = sum_of_rows[j] / input_h;

        // find std:
        std_vector[j] = sum_of_rows_square[j] - 2 * mean_vector[j] * sum_of_rows[j] + input_h * std::pow(mean_vector[j], 2.0);
        std_vector[j] = std::sqrt(std_vector[j] / (input_h - 1));

        // Normalize the matrix:
        for (int i = 0; i < input_h; ++i)
        {
            cnn_input_matrix_normalized[i * input_w + j] = (cnn_input_matrix[i][j] - mean_vector[j]) / std_vector[j];
        }
        previous_first_row[j] = cnn_input_matrix[0][j];
    }

    cnn_input_queue.push(cnn_input_matrix_normalized);
}


