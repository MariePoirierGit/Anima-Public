#include <animaBoundedLevenbergMarquardtOptimizer.h>
#include <animaBVLSOptimizer.h>
#include <animaBaseTensorTools.h>
#include <limits>
#include <vnl/algo/vnl_qr.h>
#include <animaQRPivotDecomposition.h>
#include <animaBLMLambdaCostFunction.h>
#include <animaNLOPTOptimizers.h>

namespace anima
{

void BoundedLevenbergMarquardtOptimizer::StartOptimization()
{
    m_CurrentPosition = this->GetInitialPosition();
    ParametersType parameters(m_CurrentPosition);

    unsigned int nbParams = parameters.size();

    MeasureType newResidualValues;

    m_CurrentValue = this->EvaluateCostFunctionAtParameters(parameters,m_ResidualValues);
    unsigned int numResiduals = m_ResidualValues.size();

    unsigned int numIterations = 0;
    bool stopConditionReached = false;
    bool rejectedStep = false;

    DerivativeType derivativeMatrix(nbParams,numResiduals);
    DerivativeType derivativeMatrixCopy;
    ParametersType oldParameters = parameters;
    ParametersType dValues(nbParams);

    // Be careful here: we consider the problem of the form |f(x)|^2, J is thus the Jacobian of f
    // If f is itself y - g(x), then J = - J_g which is what is on the wikipedia page
    m_CostFunction->GetDerivative(parameters,derivativeMatrix);
    derivativeMatrix = derivativeMatrix.transpose();
    derivativeMatrixCopy = derivativeMatrix;

    bool derivativeCheck = false;
    for (unsigned int i = 0;i < nbParams;++i)
    {
        for (unsigned int j = 0;j < numResiduals;++j)
        {
            if (std::abs(derivativeMatrix[i][j]) > std::sqrt(std::numeric_limits <double>::epsilon()))
            {
                derivativeCheck = true;
                break;
            }
        }

        if (derivativeCheck)
            break;
    }

    if (!derivativeCheck)
        return;

    m_DeltaParameter = 0.0;
    double maxDValue = 0.0;

    for (unsigned int i = 0;i < nbParams;++i)
    {
        double normValue = 0.0;
        for (unsigned int j = 0;j < numResiduals;++j)
            normValue += derivativeMatrix[j][i] * derivativeMatrix[j][i];
        
        dValues[i] = std::sqrt(normValue);
        if (dValues[i] != 0.0)
        {
            if ((i == 0) || (dValues[i] > maxDValue))
                maxDValue = dValues[i];
        }
    }
    
    double basePower = std::floor(std::log(maxDValue) / std::log(2.0));
    double epsilon = 20.0 * std::numeric_limits <double>::epsilon() * (numResiduals + nbParams) * std::pow(2.0,basePower);

    // Change the scaling d-values if they are below a threshold of matrix rank (as in QR decomposition)
    for (unsigned int i = 0;i < nbParams;++i)
    {
        if (dValues[i] < epsilon)
            dValues[i] = epsilon;

        m_DeltaParameter += dValues[i] * parameters[i] * parameters[i];
    }

    m_DeltaParameter = std::sqrt(m_DeltaParameter);

    unsigned int rank = 0;
    // indicates ones in pivot matrix as pivot(pivotVector(i),i) = 1
    std::vector <unsigned int> pivotVector(nbParams);
    // indicates ones in pivot matrix as pivot(i,inversePivotVector(i)) = 1
    std::vector <unsigned int> inversePivotVector(nbParams);
    std::vector <double> qrBetaValues(nbParams);
    ParametersType qtResiduals = m_ResidualValues;
    ParametersType lowerBoundsPermutted(nbParams);
    ParametersType upperBoundsPermutted(nbParams);
    anima::QRPivotDecomposition(derivativeMatrix,pivotVector,qrBetaValues,rank);
    anima::GetQtBFromQRDecomposition(derivativeMatrix,qtResiduals,qrBetaValues,rank);
    for (unsigned int i = 0;i < nbParams;++i)
        inversePivotVector[pivotVector[i]] = i;

    while (!stopConditionReached)
    {
        ++numIterations;

        for (unsigned int i = 0;i < nbParams;++i)
        {
            lowerBoundsPermutted[i] = m_LowerBounds[pivotVector[i]] - oldParameters[pivotVector[i]];
            upperBoundsPermutted[i] = m_UpperBounds[pivotVector[i]] - oldParameters[pivotVector[i]];
        }

        // Updates lambda and get new addon vector at the same time
        this->UpdateLambdaParameter(derivativeMatrix,dValues,pivotVector,inversePivotVector,
                                    qtResiduals,lowerBoundsPermutted,upperBoundsPermutted,rank);

        parameters = oldParameters;
        parameters += m_CurrentAddonVector;

        // Check acceptability of step, careful because EvaluateCostFunctionAtParameters returns the squared cost
        double tentativeNewCostValue = this->EvaluateCostFunctionAtParameters(parameters,newResidualValues);
        rejectedStep = (tentativeNewCostValue > m_CurrentValue);

        double acceptRatio = 0.0;

        // Compute || f + Jp ||^2
        double fjpNorm = 0.0;
        for (unsigned int i = 0;i < numResiduals;++i)
        {
            double fjpAddonValue = m_ResidualValues[i];

            for (unsigned int j = 0;j < nbParams;++j)
                fjpAddonValue += derivativeMatrixCopy[i][j] * m_CurrentAddonVector[j];

            fjpNorm += fjpAddonValue * fjpAddonValue;
        }

        if (!rejectedStep)
        {
            acceptRatio = 1.0 - tentativeNewCostValue / m_CurrentValue;

            double denomAcceptRatio = 1.0 - fjpNorm / m_CurrentValue;

            if (denomAcceptRatio > 0.0)
                acceptRatio /= denomAcceptRatio;
            else
                acceptRatio = 0.0;
        }

        if (acceptRatio >= 0.75)
        {
            // Increase Delta
            m_DeltaParameter *= 2.0;
        }
        else if (acceptRatio <= 0.25)
        {
            double mu = 0.5;
            if (tentativeNewCostValue > 100.0 * m_CurrentValue)
                mu = 0.1;
            else if (tentativeNewCostValue > m_CurrentValue)
            {
                // Gamma is p^T J^T f / |f|^2
                double gamma = 0.0;
                for (unsigned int i = 0;i < nbParams;++i)
                {
                    double jtFValue = 0.0;
                    for (unsigned int j = 0;j < numResiduals;++j)
                        jtFValue += derivativeMatrixCopy[j][i] * m_ResidualValues[i];

                    gamma += m_CurrentAddonVector[i] * jtFValue;
                }

                gamma /= m_CurrentValue;

                if (gamma < - 1.0)
                    gamma = - 1.0;
                else if (gamma > 0.0)
                    gamma = 0.0;

                mu = 0.5 * gamma;
                double denomMu = gamma + 0.5 * (1.0 - tentativeNewCostValue / m_CurrentValue);
                mu /= denomMu;

                mu = std::min(0.5,std::max(0.1,mu));
            }

            m_DeltaParameter *= mu;
        }

        if (!rejectedStep)
        {
            m_ResidualValues = newResidualValues;
            m_CostFunction->GetDerivative(parameters,derivativeMatrix);

            for (unsigned int i = 0;i < nbParams;++i)
            {
                double normValue = 0;
                for (unsigned int j = 0;j < numResiduals;++j)
                    normValue += derivativeMatrix[i][j] * derivativeMatrix[i][j];
                
                normValue = std::sqrt(normValue);
                dValues[i] = std::max(dValues[i], normValue);
            }

            derivativeMatrix = derivativeMatrix.transpose();
            derivativeMatrixCopy = derivativeMatrix;

            qtResiduals = m_ResidualValues;
            anima::QRPivotDecomposition(derivativeMatrix,pivotVector,qrBetaValues,rank);
            anima::GetQtBFromQRDecomposition(derivativeMatrix,qtResiduals,qrBetaValues,rank);
            for (unsigned int i = 0;i < nbParams;++i)
                inversePivotVector[pivotVector[i]] = i;
        }

        if (numIterations != 1)
            stopConditionReached = this->CheckConditions(numIterations,parameters,dValues,
                                                         tentativeNewCostValue);

        if (!rejectedStep)
        {
            oldParameters = parameters;
            m_CurrentValue = tentativeNewCostValue;
        }
    }

    this->SetCurrentPosition(oldParameters);
}

bool BoundedLevenbergMarquardtOptimizer::CheckSolutionIsInBounds(ParametersType &solutionVector, ParametersType &lowerBounds,
                                                                 ParametersType &upperBounds, unsigned int rank)
{
    for (unsigned int i = 0;i < rank;++i)
    {
        if (solutionVector[i] < lowerBounds[i])
            return false;

        if (solutionVector[i] > upperBounds[i])
            return false;
    }

    return true;
}

void BoundedLevenbergMarquardtOptimizer::UpdateLambdaParameter(DerivativeType &derivative, ParametersType &dValues,
                                                               std::vector <unsigned int> &pivotVector,
                                                               std::vector <unsigned int> &inversePivotVector,
                                                               ParametersType &qtResiduals, ParametersType &lowerBoundsPermutted,
                                                               ParametersType &upperBoundsPermutted, unsigned int rank)
{
    anima::BLMLambdaCostFunction::Pointer cost = anima::BLMLambdaCostFunction::New();
    cost->SetWorkMatricesAndVectorsFromQRDerivative(derivative,qtResiduals,rank);
    cost->SetJRank(rank);
    cost->SetDValues(dValues);
    cost->SetPivotVector(pivotVector);
    cost->SetInversePivotVector(inversePivotVector);
    cost->SetLowerBoundsPermutted(lowerBoundsPermutted);
    cost->SetUpperBoundsPermutted(upperBoundsPermutted);
    cost->SetDeltaParameter(m_DeltaParameter);
    cost->SetSquareCostFunction(false);

    ParametersType p(cost->GetNumberOfParameters());
    p[0] = 0.0;
    double zeroCost = cost->GetValue(p);
    if (zeroCost <= 0.0)
    {
        m_LambdaParameter = 0.0;
        m_CurrentAddonVector = cost->GetSolutionVector();
        return;
    }

    ParametersType lowerBoundLambda(1), upperBoundLambda(1);
    lowerBoundLambda[0] = 0.0;
    upperBoundLambda[0] = 0.0;

    // If full rank, compute lower bound for lambda
    unsigned int n = derivative.cols();

    // Compute upper bound for lambda
    vnl_vector <double> u0InVector(n);
    u0InVector.fill(0.0);
    for (unsigned int i = 0;i < n;++i)
    {
        u0InVector[i] = 0.0;
        for (unsigned int j = 0;j < rank;++j)
        {
            if (j <= i)
                u0InVector[i] += derivative(j,i) * qtResiduals[j];
        }
    }

    for (unsigned int i = 0;i < n;++i)
        upperBoundLambda[0] += (u0InVector[inversePivotVector[i]] / dValues[i]) * (u0InVector[inversePivotVector[i]] / dValues[i]);

    upperBoundLambda[0] = std::sqrt(upperBoundLambda[0]) / m_DeltaParameter;
    p[0] = upperBoundLambda[0] / 2.0;

    anima::NLOPTOptimizers::Pointer optimizer = anima::NLOPTOptimizers::New();

    optimizer->SetAlgorithm(NLOPT_LN_BOBYQA);

    cost->SetSquareCostFunction(true);
    optimizer->SetCostFunction(cost);

    optimizer->SetMaximize(false);
    optimizer->SetXTolRel(1.0e-3);
    optimizer->SetFTolRel(1.0e-3);
    optimizer->SetMaxEval(500);
    optimizer->SetVectorStorageSize(2000);

    optimizer->SetLowerBoundParameters(lowerBoundLambda);
    optimizer->SetUpperBoundParameters(upperBoundLambda);

    optimizer->SetInitialPosition(p);
    optimizer->StartOptimization();

    p = optimizer->GetCurrentPosition();
    m_LambdaParameter = p[0];

    cost->GetValue(p);
    m_CurrentAddonVector = cost->GetSolutionVector();
}

double BoundedLevenbergMarquardtOptimizer::EvaluateCostFunctionAtParameters(ParametersType &parameters, MeasureType &residualValues)
{
    residualValues = m_CostFunction->GetValue(parameters);

    unsigned int numResiduals = residualValues.size();
    double costValue = 0.0;
    for (unsigned int i = 0;i < numResiduals;++i)
        costValue += residualValues[i] * residualValues[i];

    return costValue;
}

bool BoundedLevenbergMarquardtOptimizer::CheckConditions(unsigned int numIterations, ParametersType &newParams,
                                                         ParametersType &dValues, double newCostValue)
{
    if (numIterations == m_NumberOfIterations)
        return true;

    // Criteria as in More, 8.3 and 8.4 equations
    double dxNorm = 0.0;
    for (unsigned int i = 0;i < newParams.size();++i)
        dxNorm += (dValues[i] * newParams[i]) * (dValues[i] * newParams[i]);

    dxNorm = std::sqrt(dxNorm);
    if (m_DeltaParameter < m_ValueTolerance * dxNorm)
        return true;

    double relativeDiff = (m_CurrentValue - newCostValue) / m_CurrentValue;

    if ((relativeDiff >= 0.0) && (relativeDiff < m_CostTolerance))
        return true;

    return false;
}

} // end namespace anima
