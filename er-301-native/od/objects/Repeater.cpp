/*
 * Repeater.cpp
 *
 *  Created on: 21 Aug 2018
 *      Author: clarkson
 */

#include <od/objects/Repeater.h>
#include <od/extras/LookupTables.h>
#include <od/config.h>
#include <od/objects/StreamingRepeaterIO.h>
#include <string.h>

namespace od
{

    Repeater::Repeater()
    {
        own(mInlet);
        own(mOutlet);
    }

    Repeater::~Repeater()
    {
    }

    void Repeater::copyInputToOutput()
    {
        float *in = mInlet.buffer();
        float *out = mOutlet.buffer();
        memcpy(out, in, sizeof(float) * FRAMELENGTH);
    }

    bool Repeater::copyRange(int startSample, int sampleCount)
    {
        return streaming::copyRepeaterRange(mInlet.buffer(), mOutlet.buffer(),
                                            FRAMELENGTH, startSample, sampleCount);
    }

    bool Repeater::fadeRange(float a, float w0, int startSample, int sampleCount)
    {
        return streaming::fadeRepeaterRange(
            mInlet.buffer(), mOutlet.buffer(),
            LookupTables::FrameOfLinearRamp.mValues.data(),
            FRAMELENGTH, startSample, sampleCount, a, w0);
    }

    void Repeater::fade(float a, float w0)
    {
        float *in = mInlet.buffer();
        float *out = mOutlet.buffer();
        float *ramp = LookupTables::FrameOfLinearRamp.mValues.data();
        for (int j = 0; j < FRAMELENGTH; j++)
        {
            out[j] = in[j] * (a * ramp[j] + w0);
        }
    }

} /* namespace od */
