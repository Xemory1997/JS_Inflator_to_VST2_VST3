//------------------------------------------------------------------------
// Copyright(c) 2023 yg331.
//------------------------------------------------------------------------

#include "InflatorPackageprocessor.h"
#include "InflatorPackagecids.h"

#include "base/source/fstreamer.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include "public.sdk/source/vst/vstaudioprocessoralgo.h"
#include "public.sdk/source/vst/vsthelpers.h"


#include "param.h"

using namespace Steinberg;

namespace yg331 {
	//------------------------------------------------------------------------
	// InflatorPackageProcessor
	//------------------------------------------------------------------------
	InflatorPackageProcessor::InflatorPackageProcessor()
	{
		//--- set the wanted controller for our processor
		setControllerClass(kInflatorPackageControllerUID);
	}

	//------------------------------------------------------------------------
	InflatorPackageProcessor::~InflatorPackageProcessor()
	{}

	//------------------------------------------------------------------------
	tresult PLUGIN_API InflatorPackageProcessor::initialize(FUnknown* context)
	{
		// Here the Plug-in will be instantiated

		//---always initialize the parent-------
		tresult result = AudioEffect::initialize(context);
		// if everything Ok, continue
		if (result != kResultOk)
		{
			return result;
		}

		//--- create Audio IO ------
		addAudioInput (STR16("Stereo In"),  Steinberg::Vst::SpeakerArr::kStereo);
		addAudioOutput(STR16("Stereo Out"), Steinberg::Vst::SpeakerArr::kStereo);

		/* If you don't need an event bus, you can remove the next line */
		addEventInput(STR16("Event In"), 1);

		return kResultOk;
	}

	//------------------------------------------------------------------------
	tresult PLUGIN_API InflatorPackageProcessor::terminate()
	{
		// Here the Plug-in will be de-instantiated, last possibility to remove some memory!

		//---do not forget to call parent ------
		return AudioEffect::terminate();
	}

	//------------------------------------------------------------------------
	tresult PLUGIN_API InflatorPackageProcessor::setActive(TBool state)
	{
		if (state)
		{
			sendTextMessage("InflatorPackageProcessor::setActive (true)");
		}
		else
		{
			sendTextMessage("InflatorPackageProcessor::setActive (false)");
		}

		// reset the VuMeter value
		fInVuPPMLOld = 0.f;
		fInVuPPMROld = 0.f;
		fOutVuPPMLOld = 0.f;
		fOutVuPPMROld = 0.f;

		//--- called when the Plug-in is enable/disable (On/Off) -----
		return AudioEffect::setActive(state);
	}

	//------------------------------------------------------------------------
	tresult PLUGIN_API InflatorPackageProcessor::process(Vst::ProcessData& data)
	{
		//---1) Read inputs parameter changes-----------

		Vst::IParameterChanges* paramChanges = data.inputParameterChanges;

		if (paramChanges)
		{
			int32 numParamsChanged = paramChanges->getParameterCount();

			for (int32 index = 0; index < numParamsChanged; index++)
			{
				Vst::IParamValueQueue* paramQueue = paramChanges->getParameterData(index);

				if (paramQueue)
				{
					Vst::ParamValue value;
					int32 sampleOffset;
					int32 numPoints = paramQueue->getPointCount();

					/*/*/
					if (paramQueue->getPoint(numPoints - 1, sampleOffset, value) == kResultTrue) {
						switch (paramQueue->getParameterId()) {
						case kParamInput:
							fInput = (float)value;
							break;
						case kParamEffect:
							fEffect = (float)value;
							wet = fEffect;
							dry = 1.0 - fEffect;
							dry2 = dry * 2.0;
							break;
						case kParamCurve:
							fCurve = (float)value;
							curvepct = fCurve;
							curveA = 1.5 + curvepct;			// 1 + (curve + 50) / 100
							curveB = -(curvepct + curvepct);	// - curve / 50
							curveC = curvepct - 0.5;			// (curve - 50) / 100
							curveD = 0.0625 - curvepct * 0.25 + (curvepct * curvepct) * 0.25;	// 1 / 16 - curve / 400 + curve ^ 2 / (4 * 10 ^ 4)
							break;
						case kParamClip:
							bClip = (value > 0.5f);
							break;
						case kParamOutput:
							fOutput = (float)value;
							break;
						case kParamBypass:
							bBypass = (value > 0.5f);
						}
					}
				}
			}
		}

		//--- Here you have to implement your processing

		if (data.numInputs == 0 || data.numOutputs == 0) {
			return kResultOk;
		}

		// (simplification) we suppose in this example that we have the same input channel count than the output
		int32 numChannels = data.inputs[0].numChannels;

		//---get audio buffers----------------
		uint32 sampleFramesSize = getSampleFramesSizeInBytes(processSetup, data.numSamples);
		void** in  = getChannelBuffersPointer(processSetup, data.inputs[0]);
		void** out = getChannelBuffersPointer(processSetup, data.outputs[0]);

		bool chk = true;

		//---check if silence---------------
		// normally we have to check each channel (simplification)
		for (int32 ch = 0; ch < numChannels; ch++) {
			if (data.inputs[ch].silenceFlags != 0)
			{
				// mark output silence too (it will help the host to propagate the silence)
				data.outputs[ch].silenceFlags = data.inputs[ch].silenceFlags;

				// the plug-in has to be sure that if it sets the flags silence that the output buffer are
				// clear
					// do not need to be cleared if the buffers are the same (in this case input buffer are
					// already cleared by the host)
				if (in[ch] != out[ch])
				{
					memset(out[ch], 0, sampleFramesSize);
				}
			}
			// mark our outputs has not silents
			else {
				data.outputs[ch].silenceFlags = 0;
				chk = false;
			}
		}
		if (chk) { // nothing to do at this point
			return kResultOk;
		}


		float fInVuPPML = 0.f;
		float fInVuPPMR = 0.f;
		float fOutVuPPML = 0.f;
		float fOutVuPPMR = 0.f;

		fIn_db  = expf(logf(10.f) * (24.0 * fInput  - 12.0) / 20.f);
		fOut_db = expf(logf(10.f) * (12.0 * fOutput - 12.0) / 20.f);
		

		//---in bypass mode outputs should be like inputs-----
		if (bBypass)
		{
			for (int32 ch = 0; ch < numChannels; ch++)
			{
				// do not need to be copied if the buffers are the same
				if (in[ch] != out[ch])
				{
					memcpy(out[ch], in[ch], sampleFramesSize);
				}
			}

			if (data.symbolicSampleSize == Vst::kSample32) {
										fInVuPPML = processInVuPPM((Vst::Sample32**)in, (int32)0, data.numSamples);
				if (numChannels > 1)	fInVuPPMR = processInVuPPM((Vst::Sample32**)in, (int32)1, data.numSamples);
				else					fInVuPPMR = processInVuPPM((Vst::Sample32**)in, (int32)0, data.numSamples);
			}
			else {
										fInVuPPML = processInVuPPM((Vst::Sample64**)in, (int32)0, data.numSamples);
				if (numChannels > 1)	fInVuPPMR = processInVuPPM((Vst::Sample64**)in, (int32)1, data.numSamples);
				else					fInVuPPMR = processInVuPPM((Vst::Sample64**)in, (int32)0, data.numSamples);
			}
			fOutVuPPML = fInVuPPML;
			fOutVuPPMR = fInVuPPMR;
		}
		else
		{
			if (data.symbolicSampleSize == Vst::kSample32) {
										fInVuPPML = processInVuPPM((Vst::Sample32**)in, (int32)0, data.numSamples);
				if (numChannels > 1)	fInVuPPMR = processInVuPPM((Vst::Sample32**)in, (int32)1, data.numSamples);
				else					fInVuPPMR = processInVuPPM((Vst::Sample32**)in, (int32)0, data.numSamples);
				processAudio((Vst::Sample32**)in, (Vst::Sample32**)out, numChannels, data.numSamples);
										fOutVuPPML = processOutVuPPM((Vst::Sample32**)out, (int32)0, data.numSamples);
				if (numChannels > 1)	fOutVuPPMR = processOutVuPPM((Vst::Sample32**)out, (int32)1, data.numSamples);
				else					fOutVuPPMR = processOutVuPPM((Vst::Sample32**)out, (int32)0, data.numSamples);
			}
			else {
										fInVuPPML = processInVuPPM((Vst::Sample64**)in, (int32)0, data.numSamples);
				if (numChannels > 1)	fInVuPPMR = processInVuPPM((Vst::Sample64**)in, (int32)1, data.numSamples);
				else					fInVuPPMR = processInVuPPM((Vst::Sample64**)in, (int32)0, data.numSamples);
				processAudio((Vst::Sample64**)in, (Vst::Sample64**)out, numChannels, data.numSamples);
										fOutVuPPML = processOutVuPPM((Vst::Sample64**)out, (int32)0, data.numSamples);
				if (numChannels > 1)	fOutVuPPMR = processOutVuPPM((Vst::Sample64**)out, (int32)1, data.numSamples);
				else					fOutVuPPMR = processOutVuPPM((Vst::Sample64**)out, (int32)0, data.numSamples);
			}
		}

		//---3) Write outputs parameter changes-----------
		Vst::IParameterChanges* outParamChanges = data.outputParameterChanges;
		// a new value of VuMeter will be send to the host
		// (the host will send it back in sync to our controller for updating our editor)
		if (outParamChanges && (fInVuPPMLOld != fInVuPPML || fInVuPPMROld != fInVuPPMR || fOutVuPPMLOld != fOutVuPPML || fOutVuPPMLOld != fOutVuPPMR) )
		{
			int32 index = 0;
			Vst::IParamValueQueue* paramQueue = outParamChanges->addParameterData(kParamInVuPPML, index);
			if (paramQueue)
			{
				int32 index2 = 0;
				paramQueue->addPoint(0, fInVuPPML, index2);
			}
			index = 0;
			paramQueue = outParamChanges->addParameterData(kParamInVuPPMR, index);
			if (paramQueue)
			{
				int32 index2 = 0;
				paramQueue->addPoint(0, fInVuPPMR, index2);
			}
			index = 0;
			paramQueue = outParamChanges->addParameterData(kParamOutVuPPML, index);
			if (paramQueue)
			{
				int32 index2 = 0;
				paramQueue->addPoint(0, fOutVuPPML, index2);
			}
			index = 0;
			paramQueue = outParamChanges->addParameterData(kParamOutVuPPMR, index);
			if (paramQueue)
			{
				int32 index2 = 0;
				paramQueue->addPoint(0, fOutVuPPMR, index2);
			}

		}
		fInVuPPMLOld = fInVuPPML;
		fInVuPPMROld = fInVuPPMR;
		fOutVuPPMLOld = fOutVuPPMR;
		fOutVuPPMLOld = fOutVuPPMR;

		return kResultOk;
	}

	//------------------------------------------------------------------------
	tresult PLUGIN_API InflatorPackageProcessor::setupProcessing(Vst::ProcessSetup& newSetup)
	{
		//--- called before any processing ----
		return AudioEffect::setupProcessing(newSetup);
	}

	//------------------------------------------------------------------------
	tresult PLUGIN_API InflatorPackageProcessor::canProcessSampleSize(int32 symbolicSampleSize)
	{
		// by default kSample32 is supported
		if (symbolicSampleSize == Vst::kSample32)
			return kResultTrue;

		// disable the following comment if your processing support kSample64
		if (symbolicSampleSize == Vst::kSample64)
			return kResultTrue; 

		return kResultFalse;
	}

	//------------------------------------------------------------------------
	tresult PLUGIN_API InflatorPackageProcessor::setState(IBStream* state)
	{
		// called when we load a preset, the model has to be reloaded
		IBStreamer streamer(state, kLittleEndian);

		float savedInput = 0.f;
		if (streamer.readFloat(savedInput) == false)
			return kResultFalse;

		float savedEffect = 0.f;
		if (streamer.readFloat(savedEffect) == false)
			return kResultFalse;

		float savedCurve = 0.f;
		if (streamer.readFloat(savedCurve) == false)
			return kResultFalse;

		int32 savedClip = 0;
		if (streamer.readInt32(savedClip) == false)
			return kResultFalse;

		float savedOutput = 0.f;
		if (streamer.readFloat(savedOutput) == false)
			return kResultFalse;

		int32 savedBypass = 0;
		if (streamer.readInt32(savedBypass) == false)
			return kResultFalse;

		fInput = savedInput;
		fEffect = savedEffect;
		fCurve = savedCurve;
		bClip = savedClip > 0;
		fOutput = savedOutput;
		bBypass = savedBypass > 0;

		if (Vst::Helpers::isProjectState(state) == kResultTrue)
		{
			// we are in project loading context...

			// Example of using the IStreamAttributes interface
			FUnknownPtr<Vst::IStreamAttributes> stream(state);
			if (stream)
			{
				if (Vst::IAttributeList* list = stream->getAttributes())
				{
					// get the full file path of this state
					Vst::TChar fullPath[1024];
					memset(fullPath, 0, 1024 * sizeof(Vst::TChar));
					if (list->getString(Vst::PresetAttributes::kFilePathStringType, fullPath,
						1024 * sizeof(Vst::TChar)) == kResultTrue)
					{
						// here we have the full path ...
					}
				}
			}
		}


		return kResultOk;
	}

	//------------------------------------------------------------------------
	tresult PLUGIN_API InflatorPackageProcessor::getState(IBStream* state)
	{
		// here we need to save the model
		IBStreamer streamer(state, kLittleEndian);

		streamer.writeFloat(fInput);
		streamer.writeFloat(fEffect);
		streamer.writeFloat(fCurve);
		streamer.writeInt32(bClip ? 1 : 0);
		streamer.writeFloat(fOutput);
		streamer.writeInt32(bBypass ? 1 : 0);

		return kResultOk;
	}

	//------------------------------------------------------------------------

	template <typename SampleType, typename num>
	bool InflatorPackageProcessor::processAudio(SampleType** input, SampleType** output, num numChannels, num sampleFrames)
	{
		for (num ch = 0; ch < numChannels; ch++)
		{
			num samples = sampleFrames;
			SampleType* ptrIn  = (SampleType*)input[ch];
			SampleType* ptrOut = (SampleType*)output[ch];
			SampleType tmp;

			SampleType each_in;
			SampleType each_out;
			SampleType s_1;
			SampleType s_2;
			SampleType s_3;
			SampleType s;

			SampleType sign;

			while (--samples >= 0)
			{
				each_in = *ptrIn * fIn_db;
				if (bClip) each_in = max((SampleType)-1.0, min((SampleType)1.0, each_in));
				(each_in > 0.0) ? (sign = 1.0) : (sign = -1.0);
				s_1 = abs(each_in);
				s_2 = s_1 * s_1;
				s_3 = s_2 * s_1;
				s = (s_1 >= 2.0) ? (
					0.0
					) : (s_1 > 1.0) ? (
						2.0 * s_1 - s_2
						) : (
							curveA * s_1 + curveB * s_2 + curveC * s_3 - curveD * (s_2 - 2.0 * s_3 + s_2 * s_2)
							);



				each_out = sign * s * wet + min(max(each_in * dry, (SampleType) -dry2), (SampleType) dry2);
				if (bClip) each_out = max((SampleType)-1.0, min((SampleType)1.0, each_out));
				tmp = each_out * fOut_db;
				*ptrOut = tmp;

				ptrIn++;
				ptrOut++;
			}
		}
		return kResultOk;
	}

	template <typename SampleType>
	SampleType InflatorPackageProcessor::processInVuPPM(SampleType** input, int32 ch, int32 sampleFrames)
	{
		SampleType vuPPM = 0.0;

		SampleType bg = (bBypass) ? 1.0 : fIn_db;

		int32 samples = sampleFrames;
		SampleType* ptrIn = (SampleType*)input[ch];
		SampleType tmp;

		while (--samples >= 0)
		{
			tmp = bg * (*ptrIn++);

			// check only positive values
			if (tmp > vuPPM)
			{
				vuPPM = tmp;
			}
		}
		vuPPM = ((20 * log10f((float)(vuPPM))) + 48.0) / 60.0;
		return vuPPM;
	}

	template <typename SampleType>
	SampleType InflatorPackageProcessor::processOutVuPPM(SampleType** output, int32 ch, int32 sampleFrames)
	{
		SampleType vuPPM = 0;

		int32 samples = sampleFrames;
		SampleType* ptrOut = (SampleType*)output[ch];
		SampleType tmp;

		while (--samples >= 0)
		{
			tmp = (*ptrOut++);

			// check only positive values
			if (tmp > vuPPM)
			{
				vuPPM = tmp;
			}
		}
		vuPPM = ((20 * log10f((float)(vuPPM))) + 48.0) / 60.0;
		return vuPPM;
	}

} // namespace yg331
