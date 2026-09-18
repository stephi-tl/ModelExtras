#include "pch.h"
#include "headlight.h"
#include "utils/util.h"
#include "utils/car.h"
#include "utils/audiomgr.h"
#include "utils/render.h"
#include "../damage.h"
#include <CWeather.h>
#include <CBike.h>

extern bool gbProperShadersDetected;
extern bool gbSkyGfxDeferredDetected;

struct SkyGfxExternalHeadlight
{
    void *vehicle;
    int side;
    float pos[3];
    float dir[3];
    float color[3];
    float intensityMul;
    float rangeMul;
    int colorOverride;
};

using SkyGfxRegisterDeferredHeadlightFn = void (__cdecl *)(const SkyGfxExternalHeadlight *);

static SkyGfxRegisterDeferredHeadlightFn GetSkyGfxDeferredRegistrar()
{
    static SkyGfxRegisterDeferredHeadlightFn fn = nullptr;
    if (fn)
        return fn;

    HMODULE hSkyGfx = GetModuleHandleA("skygfx.asi");
    if (!hSkyGfx)
        return nullptr;

    fn = reinterpret_cast<SkyGfxRegisterDeferredHeadlightFn>(
        GetProcAddress(hSkyGfx, "SkyGfx_RegisterDeferredHeadlight"));
    return fn;
}

static void RegisterSkyGfxHeadlight(CVehicle *pVeh, VehicleDummy &dummy, int side,
                                    float intensityMul, float rangeMul)
{
    auto fn = GetSkyGfxDeferredRegistrar();
    if (!fn || !pVeh)
        return;

    dummy->Update();
    DummyConfig &c = dummy->Get();
    if (!c.frame)
        return;

    RwFrame *parent = RwFrameGetParent(c.frame);
    bool isBike = pVeh->m_nVehicleSubClass == VEHICLE_BIKE;
    bool isDamaged = false;
    if (c.damagePanel != -1 || c.damageDoor != -1) {
        isDamaged = CarUtil::IsDummyDamaged(pVeh, c);
    } else if (parent) {
        isDamaged = Util::IsFrameDamaged(pVeh, parent);
    }
    if (!isDamaged && parent) {
        isDamaged = !FrameUtil::IsOkAtomicVisible(parent);
    }
    if (!isBike && pVeh->GetIsOnScreen() && isDamaged)
        return;

    CMatrix dummyMat = *(CMatrix *)&c.frame->ltm;

    CVector lightPos = pVeh->TransformFromObjectSpace(c.shadow.position);
    if (isBike && c.leanAffected)
    {
        CBike *pBike = static_cast<CBike *>(pVeh);
        bool wasCalculated = pBike->m_bLeanMatrixCalculated;
        if (!wasCalculated)
            pBike->CalculateLeanMatrix();
        lightPos = pBike->m_mLeanMatrix * c.shadow.position;
        pBike->m_bLeanMatrixCalculated = wasCalculated;
    }

    CMatrix vehMat = pVeh->GetMatrix();
    CVector localDir;
    localDir.x = CVector::Dot(dummyMat.up, vehMat.right);
    localDir.y = CVector::Dot(dummyMat.up, vehMat.up);
    localDir.z = CVector::Dot(dummyMat.up, vehMat.at);
    if (c.mirroredX)
        localDir.x = -localDir.x;

    if (localDir.Magnitude() < 0.1f || localDir.y <= 0.0f)
        localDir = CVector(0.0f, 1.0f, -0.10f);
    localDir.Normalize();

    CVector lightDir = vehMat.right * localDir.x + vehMat.up * localDir.y + vehMat.at * localDir.z;
    lightDir.Normalize();

    SkyGfxExternalHeadlight out{};
    out.vehicle = pVeh;
    out.side = side;
    out.pos[0] = lightPos.x;
    out.pos[1] = lightPos.y;
    out.pos[2] = lightPos.z;
    out.dir[0] = lightDir.x;
    out.dir[1] = lightDir.y;
    out.dir[2] = lightDir.z;
    out.color[0] = c.corona.color.r / 255.0f;
    out.color[1] = c.corona.color.g / 255.0f;
    out.color[2] = c.corona.color.b / 255.0f;
    out.intensityMul = std::clamp(intensityMul, 0.0f, 2.0f);
    out.rangeMul = std::clamp(rangeMul, 0.25f, 4.0f);
    out.colorOverride = c.hasCustomColor ? 1 : 0;
    fn(&out);
}

void HeadlightComponent::RegisterMaterials(std::unordered_map<uint32_t, eMaterialType>& matMap) {
    matMap[VEHCOL_HEADLIGHT_LEFT.ToInt()] = eMaterialType::HeadLightLeft;
    matMap[VEHCOL_HEADLIGHT_RIGHT.ToInt()] = eMaterialType::HeadLightRight;
}

eMaterialType HeadlightComponent::GetMatType(CRGBA matCol) {
    if (matCol == VEHCOL_HEADLIGHT_LEFT) return eMaterialType::HeadLightLeft;
    if (matCol == VEHCOL_HEADLIGHT_RIGHT) return eMaterialType::HeadLightRight;
    return eMaterialType::UnknownMaterial;
}

static bool CanVehicleHaveHeadlights(CVehicle* pVeh) {
    if (!pVeh) return false;
    int model = pVeh->m_nModelIndex;
    if (CModelInfo::IsBmxModel(model) || CModelInfo::IsBoatModel(model) || CModelInfo::IsTrailerModel(model) || CModelInfo::IsHeliModel(model) || CModelInfo::IsPlaneModel(model)) {
        return false;
    }
    if (pVeh->m_nVehicleSubClass == VEHICLE_BMX || pVeh->m_nVehicleSubClass == VEHICLE_BOAT || pVeh->m_nVehicleSubClass == VEHICLE_TRAILER || pVeh->m_nVehicleSubClass == VEHICLE_HELI || pVeh->m_nVehicleSubClass == VEHICLE_PLANE) {
        return false;
    }
    return true;
}

bool HeadlightComponent::AreHeadlightsOpen(CVehicle* pVeh, const VehLightData& data) {
    if (!pVeh || pVeh->m_nVehicleSubClass != VEHICLE_AUTOMOBILE) {
        return true;
    }

    CAutomobile* pAuto = static_cast<CAutomobile*>(pVeh);
    bool hasPopUp = (pVeh->m_nModelIndex == MODEL_ZR350 && pAuto->m_aCarNodes[CAR_MISC_A] != nullptr) || data.bHasVehFuncsPopUp;
    if (!hasPopUp) {
        return true;
    }

    return pAuto->m_renderLights.m_bLeftFront || pAuto->m_renderLights.m_bRightFront || pAuto->m_fPropRotate >= 0.68f;
}

bool HeadlightComponent::TryRegisterDummy(CVehicle* pVeh, RwFrame* pFrame, const std::string_view name, VehLightData& data) {
    if (!CanVehicleHaveHeadlights(pVeh)) return false;
    if (name == "headlights" || name == "headlights2") {
        if (pFrame && !rwLinkListEmpty(&pFrame->objectList)) {
            return false;
        }
        DummyConfig c = LightManager::CreateBaseConfig(pVeh, pFrame);
        c.dummyPos = eDummyPos::Front;
        c.lightType = eMaterialType::HeadLightLeft;
        c.corona.size = LightsConfig::Get().gfHeadLightCoronaSize;
        c.corona.color = {250, 250, 250, static_cast<unsigned char>(LightsConfig::Get().gHeadLightCoronaIntensity)};
        c.shadow.color = {250, 250, 250, static_cast<unsigned char>(LightsConfig::Get().gHeadLightShadowIntensity)};
        c.shadow.size = LightsConfig::Get().gfHeadLightShadowSize;
        c.corona.lightingType = eLightingMode::Directional;
        c.shadow.render = name != "headlights2";
        
        c.mirroredX = true;
        data.dummies[eMaterialType::HeadLightLeft].push_back(VehicleDummy(c));
        
        if (pVeh->m_nVehicleSubClass != VEHICLE_BIKE || std::abs(c.frame->modelling.pos.x) > 0.05f) {
            c.mirroredX = false;
            c.lightType = eMaterialType::HeadLightRight;
            data.dummies[eMaterialType::HeadLightRight].push_back(VehicleDummy(c));
        }
        return true;
    }

    std::string lowerName;
    lowerName.reserve(name.size());
    for (char ch : name) {
        lowerName.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }

    if (lowerName.find("f_pop") != std::string::npos || lowerName.find("popup") != std::string::npos) {
        data.bHasVehFuncsPopUp = true;
        return true;
    }
    return false;
}

void HeadlightComponent::Process(CVehicle* pVeh, VehLightData& data) {
    if (!CanVehicleHaveHeadlights(pVeh)) return;

    bool isHeadlightsActive = (pVeh->bLightsOn || CarUtil::IsLightsForcedOn(pVeh) || (Util::IsNightTime() && !Util::IsEngineOff(pVeh))) && !CarUtil::IsLightsForcedOff(pVeh);
    if (pVeh->IsDriver(FindPlayerPed())) {
        if (!isHeadlightsActive && data.fLightFactor[eMaterialType::HeadLightLeft] <= 0.001f) {
            data.bLongLightsOn = false;
        }

        bool canToggleLongLights = gbSkyGfxDeferredDetected ||
            !(gbProperShadersDetected && !LightsConfig::Get().gbLightPointLights);
        if (InputMgr::IsKeyJustDown(LightsConfig::Get().nLongLightKey) && isHeadlightsActive && canToggleLongLights) {
            data.bLongLightsOn = !data.bLongLightsOn;
            AudioMgr::PlaySwitchSound(pVeh);
        }
    } else if (pVeh->m_fHealth > 0.0f) {
        if (CarUtil::IsLightsForcedOff(pVeh) || (Util::IsEngineOff(pVeh) && !CarUtil::IsLightsForcedOn(pVeh) && !pVeh->bLightsOn)) {
            return;
        }

        if (CVector::Distance(pVeh->GetPosition(), TheCamera.GetPosition()) < 300.0f || pVeh->GetIsOnScreen()) {
            bool isLeftFrontOk = !Util::IsLightDamaged(pVeh, eLights::LIGHT_FRONT_LEFT);
            bool isRightFrontOk = !Util::IsLightDamaged(pVeh, eLights::LIGHT_FRONT_RIGHT);

            if (isHeadlightsActive && AreHeadlightsOpen(pVeh, data)) {
                bool isFoggy = Util::IsFoggy();
                std::string texName = data.bLongLightsOn ? "headlight_long" : "headlight_short";
                bool shadow = !(gbProperShadersDetected || gbSkyGfxDeferredDetected);
                bool highlight = isFoggy || data.bLongLightsOn;

                LightManager::RenderLight(pVeh, data, eMaterialType::HeadLightLeft, isLeftFrontOk, shadow ? texName : "", LightsConfig::Get().headlightSz, highlight);
                LightManager::RenderLight(pVeh, data, eMaterialType::HeadLightRight, isRightFrontOk, shadow ? texName : "", LightsConfig::Get().headlightSz, highlight);
                data.nHeadlightTickFrame = CTimer::m_FrameCounter;
            }
        }
    }
}

void HeadlightComponent::Render(CVehicle* pControlVeh, CVehicle* pTowedVeh, VehLightData& data) {
    if (!CanVehicleHaveHeadlights(pControlVeh)) {
        return;
    }

    bool isNightOrOn = (pControlVeh->bLightsOn || CarUtil::IsLightsForcedOn(pControlVeh) || (Util::IsNightTime() && !Util::IsEngineOff(pControlVeh))) && !CarUtil::IsLightsForcedOff(pControlVeh);
    if (!isNightOrOn || !AreHeadlightsOpen(pControlVeh, data)) return;

    auto damage = LightDamageState::Get(pControlVeh, pTowedVeh);
    bool isHeadlightLeftOk = damage.isHeadlightLeftOk;
    bool isHeadlightRightOk = damage.isHeadlightRightOk;

    bool bTickRegistered = (data.nHeadlightTickFrame == CTimer::m_FrameCounter);
    bool isFoggy = Util::IsFoggy();
    std::string texName = data.bLongLightsOn ? "headlight_long" : "headlight_short";
    bool shadow = !(gbProperShadersDetected || gbSkyGfxDeferredDetected);
    bool highlight = isFoggy || data.bLongLightsOn;

    if (isHeadlightLeftOk || isHeadlightRightOk) {
        pControlVeh->m_renderLights.m_bLeftFront = isHeadlightLeftOk;
        pControlVeh->m_renderLights.m_bRightFront = isHeadlightRightOk;
        if (isHeadlightLeftOk) {
            LightManager::RenderLights(pControlVeh, pTowedVeh, data, eMaterialType::HeadLightLeft, true, shadow ? texName : "", LightsConfig::Get().headlightSz, highlight, true, bTickRegistered);
        }
        if (isHeadlightRightOk) {
            LightManager::RenderLights(pControlVeh, pTowedVeh, data, eMaterialType::HeadLightRight, true, shadow ? texName : "", LightsConfig::Get().headlightSz, highlight, true, bTickRegistered);
        }

        // When SkyGfx is present, feed it the exact ModelExtras headlight dummies.
        // This keeps deferred beams aligned with custom/animated/pop-up lights while
        // preserving ModelExtras coronas and emissive materials. SkyGfx de-duplicates
        // these against its vanilla fallback by vehicle + side.
        if (gbSkyGfxDeferredDetected && GetSkyGfxDeferredRegistrar()) {
            float highBeamRange = 1.0f + (LightsConfig::Get().fHighBeamPointLightMul - 1.0f) *
                                  std::clamp(data.fHighBeamFactor, 0.0f, 1.0f);

            if (isHeadlightLeftOk && data.bLightStates[eMaterialType::HeadLightLeft]) {
                float factor = data.fLightFactor[eMaterialType::HeadLightLeft];
                if (factor > 0.001f) {
                    for (auto &dummy : data.dummies[eMaterialType::HeadLightLeft])
                        RegisterSkyGfxHeadlight(pControlVeh, dummy, 0, factor, highBeamRange);
                }
            }

            if (isHeadlightRightOk && data.bLightStates[eMaterialType::HeadLightRight]) {
                float factor = data.fLightFactor[eMaterialType::HeadLightRight];
                if (factor > 0.001f) {
                    for (auto &dummy : data.dummies[eMaterialType::HeadLightRight])
                        RegisterSkyGfxHeadlight(pControlVeh, dummy, 1, factor, highBeamRange);
                }
            }
        }
    }
}

void HeadlightComponent::ProcessPointLights(CVehicle* pVeh, VehLightData& data) {
    if (gbSkyGfxDeferredDetected && GetSkyGfxDeferredRegistrar())
        return;
    if (!CanVehicleHaveHeadlights(pVeh)) return;
    bool isHeadlightsOn = (pVeh->bLightsOn || CarUtil::IsLightsForcedOn(pVeh) || (Util::IsNightTime() && !Util::IsEngineOff(pVeh))) && !CarUtil::IsLightsForcedOff(pVeh);

    if (data.bLongLightsOn && isHeadlightsOn && AreHeadlightsOpen(pVeh, data)) {
        float highBeamMul = LightsConfig::Get().fHighBeamPointLightMul;

        for (eMaterialType type : {eMaterialType::HeadLightLeft, eMaterialType::HeadLightRight}) {
            if (!LightManager::IsDummyAvailable(data, type) || !data.bLightStates[type]) {
                continue;
            }

            bool isLeft = (type == eMaterialType::HeadLightLeft);
            eLights lightEnum = isLeft ? eLights::LIGHT_FRONT_LEFT : eLights::LIGHT_FRONT_RIGHT;
            ePanels wingEnum = isLeft ? ePanels::WING_FRONT_LEFT : ePanels::WING_FRONT_RIGHT;
            if (Util::IsLightDamaged(pVeh, lightEnum) || Util::IsPanelDamaged(pVeh, wingEnum)) {
                continue;
            }

            for (auto& e : data.dummies[type]) {
                e->Update();
                RenderUtil::RegisterHeadlightPointLight(&e->Get(), highBeamMul);
            }
        }
    }
}

