#include "helpers.h"

#include <catboost/libs/data/feature_names_converter.h>
#include <catboost/libs/helpers/exception.h>
#include <catboost/libs/helpers/interrupt.h>
#include <catboost/libs/helpers/matrix.h>
#include <catboost/libs/helpers/query_info_helper.h>
#include <catboost/private/libs/options/plain_options_helper.h>
#include <catboost/private/libs/target/data_providers.h>


extern "C" PyObject* PyCatboostExceptionType;

void ProcessException() {
    try {
        throw;
    } catch (const TCatBoostException& exc) {
        PyErr_SetString(PyCatboostExceptionType, exc.what());
    } catch (const TInterruptException& exc) {
        PyErr_SetString(PyExc_KeyboardInterrupt, exc.what());
    } catch (const std::exception& exc) {
        PyErr_SetString(PyCatboostExceptionType, exc.what());
    }
}

void PyCheckInterrupted() {
    TGilGuard guard;
    if (PyErr_CheckSignals() == -1) {
        throw TInterruptException();
    }
}

void SetPythonInterruptHandler() {
    SetInterruptHandler(PyCheckInterrupted);
}

void ResetPythonInterruptHandler() {
    ResetInterruptHandler();
}

void ThrowCppExceptionWithMessage(const TString& message) {
    ythrow TCatBoostException() << message;
}

TVector<TVector<double>> EvalMetrics(
    const TFullModel& model,
    const NCB::TDataProvider& srcData,
    const TVector<TString>& metricsDescription,
    int begin,
    int end,
    int evalPeriod,
    int threadCount,
    const TString& resultDir,
    const TString& tmpDir
) {
    NPar::TLocalExecutor executor;
    executor.RunAdditionalThreads(threadCount - 1);

    TRestorableFastRng64 rand(0);

    auto metricLossDescriptions = CreateMetricLossDescriptions(metricsDescription);
    auto metrics = CreateMetrics(metricLossDescriptions, model.GetDimensionsCount());
    TMetricsPlotCalcer plotCalcer = CreateMetricCalcer(
        model,
        begin,
        end,
        evalPeriod,
        /*processedIterationsStep=*/50,
        tmpDir,
        metrics,
        &executor
    );

    auto processedDataProvider = NCB::CreateModelCompatibleProcessedDataProvider(
        srcData,
        metricLossDescriptions,
        model,
        NCB::GetMonopolisticFreeCpuRam(),
        &rand,
        &executor
    );

    if (plotCalcer.HasAdditiveMetric()) {
        plotCalcer.ProceedDataSetForAdditiveMetrics(processedDataProvider);
    }
    if (plotCalcer.HasNonAdditiveMetric()) {
        while (!plotCalcer.AreAllIterationsProcessed()) {
            plotCalcer.ProceedDataSetForNonAdditiveMetrics(processedDataProvider);
            plotCalcer.FinishProceedDataSetForNonAdditiveMetrics();
        }
    }

    TVector<TVector<double>> metricsScore = plotCalcer.GetMetricsScore();

    plotCalcer.SaveResult(resultDir, /*metricsFile=*/"", /*saveMetrics*/ false, /*saveStats=*/true).ClearTempFiles();
    return metricsScore;
}

TVector<TString> GetMetricNames(const TFullModel& model, const TVector<TString>& metricsDescription) {
    auto metrics = CreateMetricsFromDescription(metricsDescription, model.GetDimensionsCount());
    TVector<TString> metricNames;
    metricNames.reserve(metrics.ysize());
    for (auto& metric : metrics) {
        metricNames.push_back(metric->GetDescription());
    }
    return metricNames;
}

TVector<double> EvalMetricsForUtils(
    TConstArrayRef<TVector<float>> label,
    const TVector<TVector<double>>& approx,
    const TString& metricName,
    const TVector<float>& weight,
    const TVector<TGroupId>& groupId,
    const TVector<float>& groupWeight,
    const TVector<TSubgroupId>& subgroupId,
    const TVector<TPair>& pairs,
    int threadCount
) {
    NPar::TLocalExecutor executor;
    executor.RunAdditionalThreads(threadCount - 1);
    const int approxDimension = approx.ysize();
    TVector<THolder<IMetric>> metrics = CreateMetricsFromDescription({metricName}, approxDimension);
    if (!weight.empty()) {
        for (auto& metric : metrics) {
            metric->UseWeights.SetDefaultValue(true);
        }
    }
    NCB::TObjectsGrouping objectGrouping = NCB::CreateObjectsGroupingFromGroupIds<TGroupId>(
        label[0].size(),
        groupId.empty() ? Nothing() : NCB::TMaybeData<TConstArrayRef<TGroupId>>(groupId)
    );
    if (!pairs.empty()) {
        NCB::CheckPairs(pairs, objectGrouping);
    }
    TVector<TQueryInfo> queriesInfo;
    if (!groupId.empty()) {
        queriesInfo = *NCB::MakeGroupInfos(
            objectGrouping,
            subgroupId.empty() ? Nothing() : NCB::TMaybeData<TConstArrayRef<TSubgroupId>>(subgroupId),
            groupWeight.empty() ? NCB::TWeights(groupId.size()) : NCB::TWeights(TVector<float>(groupWeight)),
            TConstArrayRef<TPair>(pairs)
        ).Get();
    }
    TVector<double> metricResults;
    metricResults.reserve(metrics.size());

    TVector<const IMetric*> metricPtrs;
    metricPtrs.reserve(metrics.size());
    for (const auto& metric : metrics) {
        metricPtrs.push_back(metric.Get());
    }

    auto stats = EvalErrorsWithCaching(
        approx,
        /*approxDelts*/{},
        /*isExpApprox*/false,
        To2DConstArrayRef<float>(label),
        weight,
        queriesInfo,
        metricPtrs,
        &executor
    );

    for (auto metricIdx : xrange(metricPtrs.size())) {
        metricResults.push_back(metricPtrs[metricIdx]->GetFinalError(stats[metricIdx]));
    }
    return metricResults;
}

NJson::TJsonValue GetTrainingOptions(
    const NJson::TJsonValue& plainJsonParams,
    const NCB::TDataMetaInfo& trainDataMetaInfo,
    const TMaybe<NCB::TDataMetaInfo>& testDataMetaInfo
) {
    NJson::TJsonValue trainOptionsJson;
    NJson::TJsonValue outputFilesOptionsJson;
    NCatboostOptions::PlainJsonToOptions(plainJsonParams, &trainOptionsJson, &outputFilesOptionsJson);
    ConvertParamsToCanonicalFormat(trainDataMetaInfo, &trainOptionsJson);
    NCatboostOptions::TCatBoostOptions catboostOptions(NCatboostOptions::LoadOptions(trainOptionsJson));
    NCatboostOptions::TOutputFilesOptions outputOptions;
    outputOptions.UseBestModel.SetDefault(false);
    SetDataDependentDefaults(
        trainDataMetaInfo,
        testDataMetaInfo,
        /*continueFromModel*/ false,
        /*learningContinuation*/ false,
        &outputOptions,
        &catboostOptions
    );
    NJson::TJsonValue catboostOptionsJson;
    catboostOptions.Save(&catboostOptionsJson);
    return catboostOptionsJson;
}

size_t GetNumPairs(const NCB::TDataProvider& dataProvider) {
    size_t result = 0;
    const NCB::TMaybeData<NCB::TRawPairsData>& maybePairsData = dataProvider.RawTargetData.GetPairs();
    if (maybePairsData) {
        Visit([&](const auto& pairs) { result = pairs.size(); }, *maybePairsData);
    }
    return result;
}

TConstArrayRef<TPair> GetUngroupedPairs(const NCB::TDataProvider& dataProvider) {
    TConstArrayRef<TPair> result;
    const NCB::TMaybeData<NCB::TRawPairsData>& maybePairsData = dataProvider.RawTargetData.GetPairs();
    if (maybePairsData) {
        CB_ENSURE(
            HoldsAlternative<TFlatPairsInfo>(*maybePairsData),
            "Cannot get ungrouped pairs: pairs data is grouped"
        );
        result = Get<TFlatPairsInfo>(*maybePairsData);
    }
    return result;
}
