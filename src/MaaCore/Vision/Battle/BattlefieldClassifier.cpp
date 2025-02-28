#include "BattlefieldClassifier.h"

#include "Utils/NoWarningCV.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "Config/OnnxSessions.h"
#include "Config/TaskData.h"
#include "Utils/ImageIo.hpp"
#include "Utils/Logger.hpp"

using namespace asst;

BattlefieldClassifier::ResultOpt BattlefieldClassifier::analyze() const
{
    Result result { .object_of_interest = m_object_of_interest };
    bool analyzed = false;

    if (m_object_of_interest.skill_ready) {
        result.skill_ready = skill_ready_analyze();
        analyzed = true;
    }

    if (m_object_of_interest.deploy_direction) {
        result.deploy_direction = deploy_direction_analyze();
        analyzed = true;
    }

    if (!analyzed) {
        return std::nullopt;
    }

    return result;
}

BattlefieldClassifier::SkillReadyResult BattlefieldClassifier::skill_ready_analyze() const
{
    auto task_ptr = Task.get<MatchTaskInfo>("BattleSkillReady");
    const Rect& skill_roi_move = task_ptr->rect_move;
    Rect roi = Rect(m_base_point.x, m_base_point.y, 0, 0).move(skill_roi_move);

    cv::Mat image = make_roi(m_image, correct_rect(roi, m_image));
    std::vector<float> input = image_to_tensor(image);

    auto memory_info = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);
    constexpr int64_t batch_size = 1;
    std::array<int64_t, 4> input_shape { batch_size, image.channels(), image.cols, image.rows };

    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        memory_info,
        input.data(),
        input.size(),
        input_shape.data(),
        input_shape.size());
    SkillReadyResult::Raw raw_results;
    std::array<int64_t, 2> output_shape { batch_size, SkillReadyResult::ClsSize };
    Ort::Value output_tensor = Ort::Value::CreateTensor<float>(
        memory_info,
        raw_results.data(),
        raw_results.size(),
        output_shape.data(),
        output_shape.size());

    auto& session = OnnxSessions::get_instance().get("skill_ready_cls");
    // 这俩是hardcode在模型里的
    constexpr const char* input_names[] = { "input" };   // session.GetInputName()
    constexpr const char* output_names[] = { "output" }; // session.GetOutputName()

    Ort::RunOptions run_options;
    session.Run(run_options, input_names, &input_tensor, 1, output_names, &output_tensor, 1);
    Log.info(__FUNCTION__, "raw results:", raw_results);

    SkillReadyResult::Prob prob = softmax(raw_results);
    Log.info(__FUNCTION__, "prob:", prob);
    bool ready = prob[1] > prob[0];
    float score = std::max(prob[0], prob[1]);

#ifdef ASST_DEBUG
    if (ready) {
        rectangle(m_image_draw, make_rect<cv::Rect>(roi), cv::Scalar(0, 165, 255), 2);
        putText(m_image_draw, std::to_string(score), cv::Point(roi.x, roi.y - 10), 1, 1.2, cv::Scalar(0, 165, 255), 2);
    }
#endif

    const auto result = SkillReadyResult {
        .ready = ready,
        .rect = roi,
        .score = score,
        .raw = raw_results,
        .prob = prob,
        .base_point = m_base_point,
    };

    if (!std::filesystem::exists("DEBUG_skill_ready.txt")) {
        return result;
    }

    // 为重新训练模型截图
    static Point last_base_point = { -1, -1 };
    static auto last_save_time = std::chrono::steady_clock::now();
    static bool last_ready = false;
    const auto now = std::chrono::steady_clock::now();
    const auto duration_since_last_save =
        std::chrono::duration_cast<std::chrono::seconds>(now - last_save_time).count();

    auto need_save = false;
    // 如果相同点且结果不同，保存
    if (last_base_point == m_base_point && last_ready != ready) {
        need_save = true;
    }
    // 如果不同点且 ready，保存
    else if (last_base_point != m_base_point && ready) {
        need_save = true;
    }
    // 来点随机截图
    else if (duration_since_last_save > 10) {
        last_save_time = now;
        need_save = true;
    }

    if (need_save) {
        std::filesystem::path relative_path;
        if (ready) {
            relative_path = utils::path("debug") / utils::path("skill_ready") / utils::path("y") /
                            (utils::get_time_filestem() + "_" + std::to_string(m_base_point.x) + "_" +
                             std::to_string(m_base_point.y) + ".png");
        }
        else {
            relative_path = utils::path("debug") / utils::path("skill_ready") / utils::path("n") /
                            (utils::get_time_filestem() + "_" + std::to_string(m_base_point.x) + "_" +
                             std::to_string(m_base_point.y) + ".png");
        }
        last_base_point = m_base_point;
        last_ready = ready;
        Log.trace("Save image", relative_path);
        asst::imwrite(relative_path, image);
    }

    return result;
}

BattlefieldClassifier::DeployDirectionResult BattlefieldClassifier::deploy_direction_analyze() const
{
    const auto& task_ptr = Task.get<MatchTaskInfo>("BattleDeployDirectionRectMove");
    const Rect& roi_move = task_ptr->rect_move;
    Rect roi = Rect(m_base_point.x, m_base_point.y, 0, 0).move(roi_move);

    cv::Mat image = make_roi(m_image, correct_rect(roi, m_image));
    std::vector<float> input = image_to_tensor(image);

    auto memory_info = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);
    constexpr int64_t batch_size = 1;
    std::array<int64_t, 4> input_shape { batch_size, image.channels(), image.cols, image.rows };

    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        memory_info,
        input.data(),
        input.size(),
        input_shape.data(),
        input_shape.size());

    DeployDirectionResult::Raw raw_results;
    std::array<int64_t, 2> output_shape { batch_size, DeployDirectionResult::ClsSize };
    Ort::Value output_tensor = Ort::Value::CreateTensor<float>(
        memory_info,
        raw_results.data(),
        raw_results.size(),
        output_shape.data(),
        output_shape.size());

    auto& session = OnnxSessions::get_instance().get("deploy_direction_cls");
    // 这俩是hardcode在模型里的
    constexpr const char* input_names[] = { "input" };   // session.GetInputName()
    constexpr const char* output_names[] = { "output" }; // session.GetOutputName()

    Ort::RunOptions run_options;
    session.Run(run_options, input_names, &input_tensor, 1, output_names, &output_tensor, 1);
    Log.info(__FUNCTION__, "raw result:", raw_results);

    DeployDirectionResult::Prob prob = softmax(raw_results);
    Log.info(__FUNCTION__, "after softmax:", prob);

    size_t class_id = std::max_element(prob.begin(), prob.end()) - prob.begin();

#ifdef ASST_DEBUG
    static const std::unordered_map<size_t, std::string> ClassNames = {
        { 0, "Right" },
        { 1, "Down" },
        { 2, "Left" },
        { 3, "Up" },
    };
    if (ClassNames.size() != prob.size()) {
        Log.error("ClassNames.size() != prob.size()", ClassNames.size(), prob.size());
        throw std::runtime_error("ClassNames.size() != prob.size()");
    }
    cv::putText(
        m_image_draw,
        ClassNames.at(class_id),
        cv::Point(roi.x, roi.y + roi.height),
        cv::FONT_HERSHEY_PLAIN,
        1.2,
        cv::Scalar(0, 255, 0),
        2);
    cv::putText(
        m_image_draw,
        std::to_string(prob[class_id]),
        cv::Point(roi.x, roi.y + roi.height + 20),
        cv::FONT_HERSHEY_PLAIN,
        1.2,
        cv::Scalar(0, 255, 0),
        2);
#endif

    return DeployDirectionResult {
        .direction = static_cast<battle::DeployDirection>(class_id),
        .rect = roi,
        .score = prob[class_id],
        .raw = raw_results,
        .prob = prob,
        .base_point = m_base_point,
    };
}
