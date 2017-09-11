#include <animaGaussianMCMCost.h>
#include <cmath>

#include <animaBaseTensorTools.h>

namespace anima
{

GaussianMCMCost::MeasureType
GaussianMCMCost::GetValues(const ParametersType &parameters)
{
    unsigned int numberOfParameters = parameters.GetSize();
    unsigned int nbImages = m_Gradients.size();

    // Set MCM parameters
    m_TestedParameters.resize(numberOfParameters);

    for (unsigned int i = 0;i < numberOfParameters;++i)
        m_TestedParameters[i] = parameters[i];

    m_MCMStructure->SetParametersFromVector(m_TestedParameters);
    
    m_PredictedSignals.resize(nbImages);
    m_PredictedSquaredNorm = 0;
    double observedSquaredNorm = 0, observedPredictedProduct = 0;

    for (unsigned int i = 0;i < nbImages;++i)
    {
        double predictedSignal = m_MCMStructure->GetPredictedSignal(m_BValues[i],m_Gradients[i]);
        observedSquaredNorm += m_ObservedSignals[i] * m_ObservedSignals[i];
        m_PredictedSquaredNorm += predictedSignal * predictedSignal;
        observedPredictedProduct += m_ObservedSignals[i] * predictedSignal;
        m_PredictedSignals[i] = predictedSignal;
    }

    if (m_PredictedSquaredNorm < 1.0e-4)
    {
        std::cerr << "Squared norm of predicted signal vector: " << m_PredictedSquaredNorm << std::endl;
        itkExceptionMacro("Null predicted signal vector.");
    }

    if (m_SigmaSquare < 1.0e-4)
    {
        std::cerr << "Noise variance: " << m_SigmaSquare << std::endl;
        itkExceptionMacro("Tow low estimated noise variance.");
    }

    // Update B0
    m_B0Value = observedPredictedProduct / m_PredictedSquaredNorm;

    for (unsigned int i = 0;i < nbImages;++i)
        m_Residuals[i] = m_B0Value * m_PredictedSignals[i] - m_ObservedSignals[i];

    // Update SigmaSq
    m_SigmaSquare = (observedSquaredNorm - m_B0Value * m_B0Value * m_PredictedSquaredNorm) / nbImages;

    return m_Residuals;
}

double GaussianMCMCost::GetCurrentCostValue()
{
    // This is -2log(L) so that we only have to give one formula
    double costValue = 0;
    unsigned int nbImages = m_Residuals.size();

    if (m_MarginalEstimation)
        costValue = -2.0 * std::log(2.0) + (nbImages - 1.0) * std::log(M_PI) - 2.0 * std::log(std::tgamma((nbImages + 1.0) / 2.0)) + (nbImages + 1.0) * std::log(nbImages) + std::log(m_PredictedSquaredNorm) + (nbImages + 1.0) * std::log(m_SigmaSquare);
    else
        costValue = nbImages * (1.0 + std::log(2.0 * M_PI * m_SigmaSquare));

    return costValue;
}

void
GaussianMCMCost::GetDerivativeMatrix(const ParametersType &parameters, DerivativeMatrixType &derivative)
{
    unsigned int nbParams = parameters.GetSize();
    if (m_MarginalEstimation)
        itkExceptionMacro("Marginal estimation does not handle Levenberg-Marquardt");

    if (nbParams == 0)
        return;

    unsigned int nbValues = m_ObservedSignals.size();

    // Assume get derivative is called with the same parameters as GetValue just before
    bool requireGetValue = false;
    for (unsigned int i = 0;i < nbParams;++i)
    {
        if (m_TestedParameters[i] != parameters[i])
        {
            requireGetValue = true;
            std::cerr << "Get derivative not called with the same parameters as GetValue. Running GetValue first..." << std::endl;
            break;
        }
    }

    if (requireGetValue)
        this->GetValues(parameters);

    derivative.SetSize(nbParams, nbValues);
    derivative.Fill(0.0);

    ListType signalJacobian;
    m_PredictedJacobianProducts.resize(nbParams);
    std::fill(m_PredictedJacobianProducts.begin(),m_PredictedJacobianProducts.end(),0.0);

    for (unsigned int i = 0;i < nbValues;++i)
    {
        signalJacobian = m_MCMStructure->GetSignalJacobian(m_BValues[i], m_Gradients[i]);

        for (unsigned int j = 0;j < nbParams;++j)
        {
            derivative(j,i) = m_B0Value * m_PredictedSignals[i] * signalJacobian[j] - m_ObservedSignals[i] * signalJacobian[j];
            m_PredictedJacobianProducts[j] += m_PredictedSignals[i] * signalJacobian[j];
        }
    }
}

void
GaussianMCMCost::GetCurrentDerivative(DerivativeMatrixType &derivativeMatrix, DerivativeType &derivative)
{
    unsigned int nbParams = derivativeMatrix.rows();
    unsigned int nbValues = derivativeMatrix.columns();

    derivative.set_size(nbParams);

    for (unsigned int j = 0;j < nbParams;++j)
    {
        double jacobianProductsSum = 0;
        for (unsigned int i = 0;i < nbValues;++i)
            jacobianProductsSum += derivativeMatrix(j,i);

        if (!m_MarginalEstimation)
            derivative[j] = 2.0 * m_B0Value * jacobianProductsSum / m_SigmaSquare;
        else
            derivative[j] = 2.0 * (m_PredictedJacobianProducts[j] / m_PredictedSquaredNorm + (nbValues + 1.0) * m_B0Value * jacobianProductsSum / (nbValues * m_SigmaSquare));
    }
}
    
} // end namespace anima
