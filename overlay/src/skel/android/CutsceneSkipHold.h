#ifndef CUTSCENE_SKIP_HOLD_H
#define CUTSCENE_SKIP_HOLD_H

#include <stdint.h>

// A hold of physical A skips only a loaded formal cutscene. Any pause, focus
// or menu interruption disarms that press until the controller button is
// released. Outside a claimed cutscene press, A remains on its native route.
class CutsceneSkipHold
{
public:
	bool Update(bool buttonDown, bool formalCutscene, bool eligible,
	            int64_t nowNs, const char *sceneName, int sceneTimeMs)
	{
		const bool sceneChanged = ObserveScene(
			formalCutscene, sceneName, sceneTimeMs);
		if(!buttonDown){
			m_state = READY;
			m_consumedByCutscene = false;
			m_startedAtNs = 0;
			m_lastSampleAtNs = 0;
			m_progress = 0.0f;
			return false;
		}

		if(formalCutscene)
			m_consumedByCutscene = true;
		if(sceneChanged){
			m_state = DISARMED;
			m_progress = 0.0f;
			return false;
		}

		if(m_state == READY){
			if(!formalCutscene || !eligible){
				m_state = DISARMED;
				m_progress = 0.0f;
				return false;
			}
			m_state = HOLDING;
			m_startedAtNs = nowNs;
			m_lastSampleAtNs = nowNs;
			m_progress = 0.0f;
			return false;
		}

		if(m_state == FIRED){
			m_progress = formalCutscene && eligible ? 1.0f : 0.0f;
			return false;
		}
		if(m_state != HOLDING){
			m_progress = 0.0f;
			return false;
		}

		// A long frame gap, clock reset, pause, focus change, or menu opening
		// must not count as continuous physical intent.
		if(!formalCutscene || !eligible || nowNs <= m_lastSampleAtNs ||
		   nowNs - m_lastSampleAtNs > MAX_SAMPLE_GAP_NS){
			m_state = DISARMED;
			m_progress = 0.0f;
			return false;
		}

		m_lastSampleAtNs = nowNs;
		const int64_t elapsedNs = nowNs - m_startedAtNs;
		m_progress = elapsedNs >= HOLD_DURATION_NS ? 1.0f :
			(float)elapsedNs / (float)HOLD_DURATION_NS;
		if(elapsedNs < HOLD_DURATION_NS)
			return false;

		m_state = FIRED;
		return true;
	}

	template <typename FinishCutscene>
	bool UpdateAndFinish(bool buttonDown, bool formalCutscene, bool eligible,
	                     int64_t nowNs, const char *sceneName,
	                     int sceneTimeMs, FinishCutscene finishCutscene)
	{
		if(!Update(buttonDown, formalCutscene, eligible, nowNs,
		           sceneName, sceneTimeMs))
			return false;
		finishCutscene();
		return true;
	}

	static bool HasConflictingChord(bool menu, bool view,
	                                float leftGrip, float rightGrip)
	{
		return menu || view || leftGrip >= 0.65f || rightGrip >= 0.65f;
	}

	bool ConsumesButton(bool buttonDown) const
	{
		return buttonDown && m_consumedByCutscene;
	}

	float Progress(void) const { return m_progress; }

private:
	enum State { READY, HOLDING, DISARMED, FIRED };
	static const int64_t HOLD_DURATION_NS = 500000000LL;
	static const int64_t MAX_SAMPLE_GAP_NS = 250000000LL;

	bool ObserveScene(bool formalCutscene, const char *sceneName,
	                  int sceneTimeMs)
	{
		if(!formalCutscene){
			m_hasSceneSample = false;
			return false;
		}

		uint32_t identity = 2166136261u;
		if(sceneName){
			for(const unsigned char *p =
			    reinterpret_cast<const unsigned char*>(sceneName); *p; ++p){
				identity ^= *p;
				identity *= 16777619u;
			}
		}
		const bool changed = m_hasSceneSample &&
			(identity != m_sceneIdentity || sceneTimeMs < m_sceneTimeMs);
		m_hasSceneSample = true;
		m_sceneIdentity = identity;
		m_sceneTimeMs = sceneTimeMs;
		return changed;
	}

	State m_state = READY;
	bool m_consumedByCutscene = false;
	bool m_hasSceneSample = false;
	uint32_t m_sceneIdentity = 0;
	int m_sceneTimeMs = 0;
	int64_t m_startedAtNs = 0;
	int64_t m_lastSampleAtNs = 0;
	float m_progress = 0.0f;
};

#endif
