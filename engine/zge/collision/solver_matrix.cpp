// The dynamics solver's matrices (zge/collision/dynamics.h SolverMatrix): up to
// 6 by 6, rows of six floats whatever the size, with one scratch matrix.
// (The sweep's blocks_0015e8d8 and blocks_00162c50 by hand, the dead ones
// too.)

#include "dynamics.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "ee/field.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/collision/solver_matrix.cpp.h"


namespace zge
{
    namespace
    {
        // The solver's own matrix, for products and inverses that would
        // overwrite what they read.
        inline ee::Global<SolverMatrix, sym::scratchMatrix> g_scratchMatrix;
        ee::Ptr<SolverMatrix> scratchMatrix() { return g_scratchMatrix.get(); }
    }


    // pivotSolverMatrix: partial pivoting: each column's largest value on or below
    // the diagonal gets the diagonal, its row swapped up. `order` gets where
    // each row came from.
    void pivotSolverMatrix(ee::Ptr<SolverMatrix> matrix, ee::Ptr<uint32_t> order)
    {
        const uint32_t rows = matrix->rows;
        auto &data = matrix->data;
        if (rows != 0)
        {
            for (uint32_t row = 0;;)
            {
                order[row] = row;
                if (++row < rows)
                    continue;
                break;
            }
        }
        if (rows != 0)
        {
            for (uint32_t column = 0;;)
            {
                uint32_t pivot = column;
                ee::Float largest = ee::abs(data[column][column]);
                if (column + 1 < rows)
                {
                    for (uint32_t row = column + 1;;)
                    {
                        const ee::Float value = ee::abs(data[row][column]);
                        if (largest < value)
                        {
                            largest = value;
                            pivot = row;
                        }
                        ++row;
                        if (row < rows)
                            continue;
                        break;
                    }
                }
                if (pivot != column)
                {
                    if (column < rows)
                    {
                        for (uint32_t swapped = column;;)
                        {
                            const ee::Float held = data[pivot][swapped];
                            data[pivot][swapped] = data[column][swapped];
                            data[column][swapped] = held;
                            if (++swapped < rows)
                                continue;
                            break;
                        }
                    }
                    const uint32_t held = order[pivot];
                    order[pivot] = order[column];
                    order[column] = held;
                }
                if (++column < rows)
                    continue;
                break;
            }
        }
    }
    READABLE_FUNCTION(pivotSolverMatrix, Verified);

    // invertSolverMatrixPivoted: Gauss-Jordan with partial pivoting, on the scratch matrix,
    // with the rows' order put back at the end. Six rows the solver's:
    // what doesn't fit invertSolverMatrix3.
    void invertSolverMatrixPivoted(ee::Ptr<SolverMatrix> out, ee::Ptr<SolverMatrix> matrix)
    {
        ee::Local<uint32_t[6]> order;       // where each row of the pivoted copy came from
        ee::Local<ee::Float[6]> column;     // a column of the result, put back in order
        auto &scratch = scratchMatrix()->data;
        auto &result = out->data;
        copySolverMatrix(scratchMatrix(), matrix);
        pivotSolverMatrix(scratchMatrix(), order);
        out->rows = matrix->rows;
        out->columns = matrix->columns;
        const uint32_t rows = matrix->rows, columns = matrix->columns;
        if (rows != 0)
        {
            // The identity.
            for (uint32_t row = 0;;)
            {
                if (columns != 0)
                {
                    for (uint32_t i = 0;;)
                    {
                        result[row][i] = ee::Float(0.0f);
                        if (++i < columns)
                            continue;
                        break;
                    }
                }
                result[row][row] = ee::Float(1.0f);
                if (++row < rows)
                    continue;
                break;
            }
        }
        if (rows != 0)
        {
            for (uint32_t pivot = 0;;)
            {
                const ee::Float divisor = scratch[pivot][pivot];
                const uint32_t resultRow = order[pivot];
                if (rows != 0)
                {
                    for (uint32_t i = 0;;)
                    {
                        scratch[pivot][i] = scratch[pivot][i] / divisor;
                        result[resultRow][i] = result[resultRow][i] / divisor;
                        if (++i < rows)
                            continue;
                        break;
                    }
                }
                for (uint32_t row = 0;;)
                {
                    const uint32_t otherResultRow = order[row];
                    if (row != pivot)
                    {
                        const ee::Float factor = scratch[row][pivot];
                        for (uint32_t i = 0;;)
                        {
                            scratch[row][i] = scratch[row][i] - factor * scratch[pivot][i];
                            result[otherResultRow][i] = result[otherResultRow][i] - factor * result[resultRow][i];
                            if (++i < rows)
                                continue;
                            break;
                        }
                        ++row;
                    }
                    else
                        ++row;
                    if (row < rows)
                        continue;
                    break;
                }
                if (++pivot < rows)
                    continue;
                break;
            }
        }
        if (columns != 0)
        {
            for (uint32_t i = 0;;)
            {
                if (rows != 0)
                {
                    for (uint32_t row = 0;;)
                    {
                        column[row] = result[order[row]][i];
                        if (++row < rows)
                            continue;
                        break;
                    }
                }
                if (rows != 0)
                {
                    for (uint32_t row = 0;;)
                    {
                        result[row][i] = column[row];
                        if (++row < rows)
                            continue;
                        break;
                    }
                }
                ++i;
                if (i < columns)
                    continue;
                break;
            }
        }
    }
    READABLE_FUNCTION(invertSolverMatrixPivoted, Verified);

    // clearSolverMatrix: the numbers only: rows and columns stay.
    ee::Ptr<SolverMatrix> clearSolverMatrix(ee::Ptr<SolverMatrix> matrix)
    {
        memoryFill(&matrix->data, 0, sizeof(SolverMatrix::data));
        return matrix;
    }
    READABLE_FUNCTION(clearSolverMatrix, Verified);

    // identitySolverMatrix: rows x columns floats cleared from the data's
    // start, then each row's diagonal element 1 (rows are stored six floats
    // wide, kRowBytes). Returns the matrix.
    ee::Ptr<SolverMatrix> identitySolverMatrix(ee::Ptr<SolverMatrix> matrix)
    {
        memoryFill(ee::field(matrix, &SolverMatrix::data), 0, matrix->rows * (matrix->columns * sizeof(ee::Float)));
        for (uint32_t row = 0; row < matrix->rows; ++row)
            matrix->data[row][row] = ee::Float(1.0f);
        return matrix;
    }
    READABLE_FUNCTION(identitySolverMatrix, Draft);

    namespace
    {
        constexpr uint32_t kMaxRows = 6;
    }

    // luDecomposeSolverMatrix: Crout's LU decomposition with implicit partial
    // pivoting, Numerical Recipes' ludcmp: the matrix is copied into `out`,
    // which gets L below the diagonal and U on and above it, the rows in
    // pivoted order. order[j] is the row swapped with row j; parity is 1 for
    // an even count of swaps, -1 for odd. Returns out, or 0 (with out half
    // done) when a row is all zero. A zero pivot becomes 1e-5. (Nothing in
    // the game calls it.)
    ee::Ptr<SolverMatrix> luDecomposeSolverMatrix(ee::Ptr<SolverMatrix> out, ee::Ptr<SolverMatrix> matrix, ee::Ptr<uint32_t> order, ee::Ptr<ee::Float> parity)
    {
        ee::Float rowScale[kMaxRows]; // 1 / each row's largest magnitude
        *parity = ee::Float(1.0f);
        copySolverMatrix(out, matrix);
        auto &a = out->data;
        const uint32_t size = out->rows;
        for (uint32_t row = 0; row < size; ++row)
        {
            ee::Float largest = ee::Float(0.0f);
            for (uint32_t column = 0; column < size; ++column)
            {
                const ee::Float magnitude = ee::abs(a[row][column]);
                if (largest < magnitude)
                    largest = magnitude;
            }
            if (largest == ee::Float(0.0f))
                return ee::Ptr<SolverMatrix>();
            rowScale[row] = ee::Float(1.0f) / largest;
        }

        // The pivot row is kept from column to column: a column that is zero
        // from the diagonal down reuses the last column's (as ludcmp's imax).
        uint32_t pivot = 0;
        for (uint32_t j = 0; j < out->rows; ++j)
        {
            // U above the diagonal.
            for (uint32_t i = 0; i < j; ++i)
            {
                ee::Float sum = a[i][j];
                for (uint32_t k = 0; k < i; ++k)
                    sum = sum - a[i][k] * a[k][j];
                a[i][j] = sum;
            }
            // The diagonal and L below it, before the division; the largest
            // scaled one is the pivot.
            ee::Float largest = ee::Float(0.0f);
            for (uint32_t i = j; i < out->rows; ++i)
            {
                ee::Float sum = a[i][j];
                for (uint32_t k = 0; k < j; ++k)
                    sum = sum - a[i][k] * a[k][j];
                a[i][j] = sum;
                const ee::Float merit = rowScale[i] * ee::abs(sum);
                if (largest < merit)
                {
                    largest = merit;
                    pivot = i;
                }
            }
            if (j != pivot)
            {
                for (uint32_t k = 0; k < out->columns; ++k)
                {
                    const ee::Float held = a[pivot][k];
                    a[pivot][k] = a[j][k];
                    a[j][k] = held;
                }
                rowScale[pivot] = rowScale[j];
                *parity = -*parity;
            }
            order[j] = pivot;
            if (a[j][j] == ee::Float(0.0f))
                a[j][j] = ee::Float(1e-05f);
            if (j != out->rows - 1)
            {
                const ee::Float reciprocal = ee::Float(1.0f) / a[j][j];
                for (uint32_t i = j + 1; i < out->rows; ++i)
                    a[i][j] = reciprocal * a[i][j];
            }
        }
        return out;
    }
    READABLE_FUNCTION(luDecomposeSolverMatrix, Draft);

    // luSolveSolverMatrix: Numerical Recipes' lubksb: solves A x = b, with A
    // as luDecomposeSolverMatrix left it and its order, by forward then back
    // substitution; x replaces b. Returns 0 for a matrix that is not square,
    // else 1. (Nothing in the game calls it.)
    // Kept bug: lubksb's b[order[i]] = b[i] is missing, so after a row swap
    // the right-hand side is not permuted: a later row reads a value its
    // swap should have replaced.
    uint32_t luSolveSolverMatrix(ee::Ptr<SolverMatrix> lu, ee::Ptr<uint32_t> order, ee::Ptr<ee::Float> b)
    {
        const auto &a = lu->data;
        if (lu->rows != lu->columns)
            return 0;
        auto size = [&] { return static_cast<int32_t>(lu->rows); };
        // Forward: from the first row with a nonzero value on.
        int32_t firstNonzero = -1;
        for (int32_t i = 0; i < size(); ++i)
        {
            ee::Float sum = b[order[i]];
            if (firstNonzero != -1)
            {
                for (int32_t k = firstNonzero; k < i; ++k)
                    sum = sum - a[i][k] * b[k];
            }
            else if (sum != ee::Float(0.0f))
                firstNonzero = i;
            b[i] = sum;
        }
        // Back.
        if (size() != 0)
        {
            for (int32_t i = size() - 1; i != -1; --i)
            {
                ee::Float sum = b[i];
                for (int32_t k = i + 1; k < size(); ++k)
                    sum = sum - a[i][k] * b[k];
                b[i] = sum / a[i][i];
            }
        }
        return 1;
    }
    READABLE_FUNCTION(luSolveSolverMatrix, Draft);

    // solverMatrixMaxValue: a walk over every element of the matrix that reads
    // none of them: whatever it measured is not in this build (the name is
    // the one guess there is).
    void solverMatrixMaxValue(ee::Ptr<SolverMatrix> matrix)
    {
        for (int32_t row = 0; row < static_cast<int32_t>(matrix->rows); ++row)
            for (int32_t column = 0; column < static_cast<int32_t>(matrix->columns); ++column)
            {
            }
    }
    READABLE_FUNCTION(solverMatrixMaxValue, Draft);

    // invertSolverMatrix3: by cofactors. Nothing checks the determinant.
    void invertSolverMatrix3(ee::Ptr<SolverMatrix> out, ee::Ptr<SolverMatrix> matrix)
    {
        const auto &data = matrix->data;
        auto &result = out->data;
        const ee::Float m22 = data[2][2], m10 = data[1][0], m12 = data[1][2];
        const ee::Float m20 = data[2][0], m11 = data[1][1], m21 = data[2][1];
        const ee::Float m00 = data[0][0], m01 = data[0][1], m02 = data[0][2];
        const ee::Float cofactor10 = m12 * m20 - m10 * m22;
        const ee::Float cofactor00 = m11 * m22 - m12 * m21;
        const ee::Float cofactor20 = m10 * m21 - m11 * m20;
        out->columns = 3;
        out->rows = 3;
        // ZGE2's determinant multiplies the third element by the second cofactor again (ZGE3 corrects it to the third).
        const ee::Float thirdCofactor = game::readable::kIsDm2 ? cofactor10 : cofactor20;
        const ee::Float inverseDeterminant = ee::Float(1.0f) / (m00 * cofactor00 + m01 * cofactor10 + m02 * thirdCofactor);
        result[0][0] = inverseDeterminant * cofactor00;
        result[1][0] = inverseDeterminant * cofactor10;
        result[2][2] = inverseDeterminant * (m00 * m11 - m01 * m10);
        result[2][0] = inverseDeterminant * cofactor20;
        result[0][1] = inverseDeterminant * (m02 * m21 - m01 * m22);
        result[1][1] = inverseDeterminant * (m00 * m22 - m02 * m20);
        result[2][1] = inverseDeterminant * (m01 * m20 - m00 * m21);
        result[0][2] = inverseDeterminant * (m01 * m12 - m02 * m11);
        result[1][2] = inverseDeterminant * (m02 * m10 - m00 * m12);
    }
    READABLE_FUNCTION(invertSolverMatrix3, Verified);

    // invertSolverMatrix3or6
    ee::Ptr<SolverMatrix> invertSolverMatrix3or6(ee::Ptr<SolverMatrix> out, ee::Ptr<SolverMatrix> matrix)
    {
        if (matrix->rows == 3)
            invertSolverMatrix3(out, matrix);
        else
            invertSolverMatrixPivoted(out, matrix);
        return out;
    }
    READABLE_FUNCTION(invertSolverMatrix3or6, Verified);

    // copySolverMatrix: a whole matrix, its 6 by 6 of numbers whatever its size.
    ee::Ptr<SolverMatrix> copySolverMatrix(ee::Ptr<SolverMatrix> out, ee::Ptr<const SolverMatrix> matrix)
    {
        memoryCopy(out, matrix, sizeof(SolverMatrix));
        return out;
    }
    READABLE_FUNCTION(copySolverMatrix, Verified);

    // multiplySolverMatrices: out = a b, through the scratch matrix when out is one of
    // them (or a is b).
    ee::Ptr<SolverMatrix> multiplySolverMatrices(ee::Ptr<SolverMatrix> out, ee::Ptr<SolverMatrix> a, ee::Ptr<SolverMatrix> b)
    {
        const bool throughScratch = a == b || a == out || b == out;
        const ee::Ptr<SolverMatrix> product = throughScratch ? scratchMatrix() : out;
        const uint32_t rows = a->rows;
        product->rows = rows;
        const uint32_t columns = b->columns;
        product->columns = columns;
        const uint32_t terms = a->columns;
        const auto &fromA = a->data, &fromB = b->data;
        auto &to = product->data;
        for (uint32_t row = 0; terms != 0 && row < rows; ++row)
        {
            for (uint32_t column = 0; column < columns; ++column)
            {
                ee::Float sum = fromA[row][0] * fromB[0][column];
                for (uint32_t term = 1; term < terms; ++term)
                    sum = sum + fromA[row][term] * fromB[term][column];
                // (Adding 0 makes a -0 a 0, as the original's accumulator does.)
                to[row][column] = sum + ee::Float(0.0f);
            }
        }
        if (throughScratch)
            copySolverMatrix(out, scratchMatrix());
        return out;
    }
    READABLE_FUNCTION(multiplySolverMatrices, Verified);

    // transposeSolverMatrix: the transpose, which can run in place: shell by shell
    // from the top left, a row with the column under it, by the larger of the
    // two sizes. In place a matrix that isn't square keeps rows for both
    // sizes, as out's columns are written first; and the last diagonal value
    // (5, 5 in a 6 by 6) is never copied, the last shell having two values
    // across. Both the original's.
    void transposeSolverMatrix(ee::Ptr<SolverMatrix> out, ee::Ptr<SolverMatrix> matrix)
    {
        constexpr uint32_t kShellLengthMax = 6, kSmallestShell = 2;
        const uint32_t oldColumns = matrix->columns, oldRows = matrix->rows;
        uint32_t shells = oldRows, length = oldColumns;
        if (oldRows >= oldColumns)
        {
            shells = oldColumns;
            length = oldRows;
        }
        out->columns = oldRows;
        out->rows = matrix->columns;
        if (length - kSmallestShell < kShellLengthMax - 1)
        {
            auto &to = out->data;
            const auto &from = matrix->data;
            // Each shell starts on the diagonal, one down and right of the last.
            for (uint32_t corner = 0; length >= kSmallestShell; --length, ++corner)
            {
                if (length != kShellLengthMax && shells == 0)
                    break;
                // Each value is read before the write that could overwrite
                // it in place.
                ee::Float carried = from[corner][corner + 1];
                to[corner][corner] = from[corner][corner];
                for (uint32_t i = 1; i < length; ++i)
                {
                    const ee::Float below = from[corner + i][corner];
                    to[corner + i][corner] = carried;
                    if (i + 1 < length)
                        carried = from[corner][corner + i + 1];
                    to[corner][corner + i] = below;
                }
                --shells;
            }
        }
    }
    READABLE_FUNCTION(transposeSolverMatrix, Verified);

    namespace
    {
        struct Plus
        {
            ee::Float operator()(ee::Float a, ee::Float b) const { return a + b; }
        };
        struct Minus
        {
            ee::Float operator()(ee::Float a, ee::Float b) const { return a - b; }
        };

        // Three values of a row, the second's pair read before the first
        // is written (out can be a or b).
        template <typename Op>
        void combineThree(ee::Float *out, const ee::Float *a, const ee::Float *b)
        {
            const Op op;
            const ee::Float secondA = a[1], secondB = b[1];
            out[0] = op(a[0], b[0]);
            const ee::Float thirdA = a[2];
            out[1] = op(secondA, secondB);
            out[2] = op(thirdA, b[2]);
        }

        // addSolverMatrices and subtractSolverMatrices are the same code:
        // by the number of columns, one of three rows unrolled, or any
        // number of them. Columns over 6 do nothing.
        template <typename Op>
        ee::Ptr<SolverMatrix> combineSolverMatrices(ee::Ptr<SolverMatrix> out, ee::Ptr<SolverMatrix> a, ee::Ptr<SolverMatrix> b)
        {
            const Op op;
            const uint32_t columns = a->columns, rows = a->rows;
            const auto &fromA = a->data, &fromB = b->data;
            auto &to = out->data;
            switch (columns)
            {
            case 3:
                for (uint32_t row = 0; row < rows; ++row)
                    combineThree<Op>(to[row], fromA[row], fromB[row]);
                break;
            case 6:
                for (uint32_t row = 0; row < rows; ++row)
                {
                    combineThree<Op>(to[row], fromA[row], fromB[row]);
                    combineThree<Op>(&to[row][3], &fromA[row][3], &fromB[row][3]);
                }
                break;
            case 1:
                for (uint32_t row = 0; row < rows; ++row)
                    to[row][0] = op(fromA[row][0], fromB[row][0]);
                break;
            case 2:
            case 4:
            case 5:
                for (uint32_t row = 0; row < rows; ++row)
                {
                    for (uint32_t column = 0; column < columns; ++column)
                        to[row][column] = op(fromA[row][column], fromB[row][column]);
                }
                break;
            default: // none, or more than 6
                break;
            }
            return out;
        }
    }

    // addSolverMatrices: out = a + b
    ee::Ptr<SolverMatrix> addSolverMatrices(ee::Ptr<SolverMatrix> out, ee::Ptr<SolverMatrix> a, ee::Ptr<SolverMatrix> b)
    {
        return combineSolverMatrices<Plus>(out, a, b);
    }
    READABLE_FUNCTION(addSolverMatrices, Verified);

    // subtractSolverMatrices: out = a - b
    ee::Ptr<SolverMatrix> subtractSolverMatrices(ee::Ptr<SolverMatrix> out, ee::Ptr<SolverMatrix> a, ee::Ptr<SolverMatrix> b)
    {
        return combineSolverMatrices<Minus>(out, a, b);
    }
    READABLE_FUNCTION(subtractSolverMatrices, Verified);

}
