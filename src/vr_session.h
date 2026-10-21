#pragma once

// Sessione OpenVR. Registrarsi come applicazione "Scene" con il visore in
// standby rallenta il gioco quasi a zero (driver NVIDIA in attesa: < 300
// frame in 70 s, 4 avvii su 4 il 16/10); come "Background" no (0 su 2,
// ~42000 frame). Quindi la mod parte come Background (pose e stato del
// visore bastano) e passa a Scene solo con il visore indossato; una volta in
// Scene resta in Scene (con Link lo stato "indossato" oscilla).
//
// Ogni uso di OpenVR (VRSystem(), VRCompositor()) va fatto dentro un
// vr::session::Shared: il cambio di tipo chiude e riapre OpenVR in
// esclusiva. Non annidare due Shared nello stesso thread.
namespace vr::session {

bool Init();     // all'avvio (thread di creazione del device)
bool Ready();    // OpenVR inizializzato (di qualunque tipo)
bool IsScene();  // si possono inviare immagini al compositor
// Visore indossato (con isteresi: Idle breve non conta, vedi MonitorLoop).
bool HmdPresent();
// Cresce a ogni (re)inizializzazione di OpenVR: gli handle di SteamVR Input
// vanno richiesti di nuovo.
unsigned Generation();

class Shared {
public:
    Shared();
    ~Shared();
    // tryOnly: non aspetta un cambio di tipo in corso (thread del gioco);
    // controllare ok() prima di usare OpenVR.
    explicit Shared(bool tryOnly);
    bool ok() const { return m_ok; }
    Shared(const Shared&) = delete;
    Shared& operator=(const Shared&) = delete;

private:
    bool m_ok = true;
};

}  // namespace vr::session
