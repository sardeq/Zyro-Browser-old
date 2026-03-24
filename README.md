# 🌐 Zyro Browser

<img src="assets/Images/logo_no_text.png" alt="Zyro Logo" width="150"/>


**Zyro** is an experimental, lightweight web browser written in **C++** using **WebKitGTK**.  
The main idea behind Zyro is simple: keep the browser fast and efficient while giving full control over the UI.

Instead of relying on native toolkit widgets, Zyro renders its entire interface using **HTML, CSS, and JavaScript**. This makes theming, layout changes, and UI experiments much easier compared to traditional desktop browsers.

> This project is still under active development and mainly serves as a learning and experimentation platform.


![Version](https://img.shields.io/badge/version-0.0.1-blue)
![Platform](https://img.shields.io/badge/platform-Linux-lightgrey)
![Language](https://img.shields.io/badge/language-C%2B%2B-brightgreen)
![Engine](https://img.shields.io/badge/engine-WebKitGTK-orange)

## ✨ Features

### 🛡️ Privacy & Security
- **AdShield Blocker**  
  Built-in content filtering that blocks common trackers and ads (Google Analytics, DoubleClick, Facebook).

- **On-Disk Encryption**  
  Sensitive data like saved passwords is encrypted using **AES-256-CBC** before being written to disk.

- **Master Key Protection**  
  The encryption key itself is stored securely using:
  - Windows DPAPI on Windows
  - libsecret on Linux

- **Incognito Mode**  
  Private sessions that don’t save history, cookies, or bookmarks.

---

### ⚡ Performance & Resource Management
- **Per-Tab Process Monitoring**  
  A built-in task manager shows PID and memory (RSS) usage for each tab.

- **Memory Trimming**  
  Optional background timer that calls `malloc_trim` to release unused heap memory.

- **Configurable Cache Behavior**  
  Switch between:
  - *Web Browser mode* (better performance)
  - *Document Viewer mode* (lower memory and disk usage)

---

### 🎨 User Experience
- **HTML-Based UI**  
  Navigation bars, settings, and the new tab page are all rendered as web content.

- **Themes (Experimental)**  
  Includes themes like:
  - Midnight
  - Deep Ocean
  - Sunset Purple

- **Smart Search**  
  Live search suggestions using remote APIs combined with local history.

- **Global Media Controls**  
  Control audio/video playback from any tab without switching pages.

- **Custom Dashboard**  
  The new tab page can show system stats (CPU/RAM), shortcuts, and custom backgrounds.

---

## 🛠️ Build Requirements

### Dependencies
- CMake (3.10+)
- GTK+ 3
- WebKit2GTK 4.1
- libsoup 3
- OpenSSL
- libsecret (Linux only)

---
## Build & Run

### Build steps
```bash
git clone https://github.com/sardeq/Zyro-Browser.git
cd Zyro-Browser
mkdir build && cd build
cmake ..
make

```
### run
```bash
./ZyroBrowser
```
