# BIST Test Ortamları — Bağlantı Haritası

Source: **Üye Test Ortamları Bilgilendirme Dokümanı**, 24.03.2026 (BIST resmi).
Yerel kopya: `ouchpdfs/txt/Test_Ortamlari_Bilgilendirme_Dokumani.txt`

Bu doküman BIST'in kendi PDF'inden alınan endpoint haritasının kod içi referansıdır. Konfigürasyon dosyaları (`config/bist_*.toml`) bu adresleri kullanır.

---

## 1. Üç Test Ortamı

| Ortam | Anchor IP | Hafta sonu | Kullanım | Üye self-service |
|---|---|---|---|---|
| **Pre-Prod** | `10.57.3.57` | **Açık** | Yeni sürüm sandbox, geliştirme | `https://connectint3.borsaistanbul.com/` |
| **Prod-like** | `10.57.3.146` | Kapalı | Cert dry-run, canlı sürüm aynası | `https://connectint2.borsaistanbul.com/` |
| **T+1** | `10.57.3.208` | Açık | T+1 takas projesi | `https://connect-uat.borsaistanbul.com/` |

Ek olarak **VYK Test Ortamı** (`10.57.3.6`) — sadece Veri Yayın Kuruluşları conformance testi için, bizim kapsamımız dışı.

## 2. Kullanıcı Sınıfları

Doc 1.2'ye göre:

| Sınıf | Pre-Prod | Prod-like | T+1 |
|---|---|---|---|
| Borsa Üyesi | ✅ | ✅ | ✅ |
| HFT Firması | ✅ | ✅ | ✅ |
| VYK | ✅ | ✅ | ✅ |
| **ISV** | ✅ | ❌ | ✅ |

ISV (bağımsız yazılım sağlayıcı) → Prod-like erişimi yok. Üye broker üstünden Prod-like'a giriş gerekir.

## 3. Hesap Tanımları

- **Üye hesapları:** canlı ortamdan tüm test ortamlarına aktarılmıştır
- **ISV hesapları:** Pre-Prod + T+1'de taze test hesabı verilir
- **Teminat:** tüm hesaplara default yüksek değer atanmıştır
- **Cert default:** `DE-1` / `123456` (BIST OUCH Cert PDF'i)

## 4. Bağlantı Kanalları (Pay Piyasası)

### 4.1 OUCH

| Ortam | UEA (VPN) | COLO (eş-yerleşim) |
|---|---|---|
| Pre-Prod | `10.57.3.61` | `194.0.142.71` |
| Prod-like | `10.57.3.144` | `194.0.142.76` |
| T+1 | `10.57.3.206` | `194.0.142.92` |

**Port:** doc'ta listelenmemiş. **Üye-spesifik provision** — BIST destek talep edilince verir. `config/bist_*.local.toml` dosyasına gerçek port girilir.

### 4.2 FIX OE/RD

| Ortam | UEA | COLO |
|---|---|---|
| Pre-Prod | `10.57.3.62` | `194.0.142.72` |
| Prod-like | `10.57.3.146` | `194.0.142.78` |
| T+1 | `10.57.3.207` | `194.0.142.93` |

### 4.3 FIX Drop Copy (DC)

| Ortam | UEA | COLO |
|---|---|---|
| Pre-Prod | `10.57.3.63` | `194.0.142.74` |
| Prod-like | `10.57.3.145` | `194.0.142.77` |
| T+1 | `10.57.3.210` | `194.0.142.188` |

### 4.4 ITCH / GLIMPSE / REWINDER (Phase 3 — wire edilmedi)

#### ITCHMOLD multicast (UDP)

| Ortam | Multicast Group | Partition Portları |
|---|---|---|
| Pre-Prod | `233.113.216.65` | 21001 / 21002 / 21003 / 21004 |
| Prod-like | `233.113.216.144` | 21001 / 21002 / 21003 / 21004 |
| T+1 | `233.113.240.71` | 21001 / 21002 / 21003 / 21004 |

#### ITCHSOUP (SoupBinTCP, kullanıcı kodu 6 karakter + şifre)

| Ortam | UEA | COLO | Partition Portları |
|---|---|---|---|
| Pre-Prod | `10.57.3.61` | `194.0.142.71` | 21501 / 21502 / 21503 / 21504 |
| Prod-like | `10.57.3.144` | `194.0.142.76` | 21501 / 21502 / 21503 / 21504 |
| T+1 | `10.57.3.206` | `194.0.142.92` | 21501 / 21502 / 21503 / 21504 |

#### ITCHRW (REWINDER, MoldUDP64 unicast)

| Ortam | Host (COLO only) | Partition Portları |
|---|---|---|
| Pre-Prod | `194.0.142.71` | 24001 / 24002 / 24003 / 24004 |
| Prod-like | `194.0.142.76` | 24001 / 24002 / 24003 / 24004 |
| T+1 | `194.0.142.92` | 24001 / 24002 / 24003 / 24004 |

#### GLIMPSE (SoupBinTCP snapshot)

| Ortam | UEA Portları | COLO Portları |
|---|---|---|
| Pre-Prod | 21802 / 21804 / 21806 / 21808 | 21801 / 21803 / 21805 / 21807 |
| Prod-like | aynı | aynı |
| T+1 | aynı | aynı |

UEA hostlar ITCHSOUP UEA host'u ile aynı.
COLO hostlar ITCHSOUP COLO host'u ile aynı.

### 4.5 PTRM GUI

| Ortam | URL'ler |
|---|---|
| Pre-Prod | `http://10.57.3.61:8082/grx/`, `http://10.57.3.62:8082/grx/` |
| Prod-like | `http://10.57.3.144:8082/grx/`, `http://10.57.3.146:8082/grx/` |
| T+1 | `http://10.57.3.206:8081/grx/`, `:8082/grx/`, `http://10.57.3.207:8081/grx/`, `:8082/grx/` |

PTRM API: talep üzerine BIST verir. Detay: BIST PTRM API Üye Duyurusu II.

### 4.6 Veri Yayın (TIP) — Phase 3 referans

| Ortam | UEA | Veri Yayın Test |
|---|---|---|
| Pre-Prod | `10.57.3.68` | `185.76.203.246` |
| Prod-like | `10.57.3.141` | `185.76.203.247` |
| T+1 | `10.57.3.214` | `185.76.203.252` |
| VYK | `10.57.3.6` | `185.76.203.242` |

Port: 39101 veya 39103 (SoupBinTCP).

### 4.7 VAS (Veri Analitikleri Sistemi) — sadece Pre-Prod

`185.76.203.251:25350` ve `185.76.203.249:35350`.

## 5. Pay Piyasası Seans Saatleri

### Prod-like + Pre-Prod (Pay Piyasası — Tek Fiyat İşlem Yöntemi, Tam İş Günü)

| Faz | Saat |
|---|---|
| Açılış emir toplama | 10:30–10:55 |
| Açılış fiyat belirleme | ~10:55 |
| Sürekli işlem (1) | 11:00–15:55 |
| Gün ortası emir toplama | 16:00–16:30 |
| Gün ortası fiyat belirleme | ~16:30 |
| Sürekli işlem (2) | 16:35–19:55 |
| Kapanış marj yayını | 20:00–20:01 |
| Kapanış emir toplama | 20:01–20:15 |
| Kapanış fiyatından / son fiyattan işlemler | 20:18–20:30 |

### T+1 (Sürekli İşlem Yöntemi)

| Faz | Saat |
|---|---|
| Açılış emir toplama | 09:40–09:55 |
| Sürekli işlem | 10:00–18:00 |
| Kapanış marj yayını | 18:00–18:01 |
| Kapanış emir toplama | 18:01–18:05 |
| Kapanış fiyatından / son fiyattan | 18:07–18:10 |

**Yarım iş günleri farklıdır** — doc'a bak (Pre-Prod sayfa 11-14).

## 6. Erişim İçin Başvuru Süreci

```
1. BIST üye veya ISV statüsü sağla
   - Üye: zaten kayıtlı, hesaplar otomatik aktarılmış
   - ISV: aracı kurum sponsoruyla başvur (1-2 ay)

2. E-posta:
   bistechsupport_autoticket@borsaistanbul.com
   Talep: "Pre-Prod / Prod-like / T+1 erişim"

3. VPN credentials BIST verir
   - Internet üzerinden Uzak Erişim Ağı (UEA)
   - COLO için ayrı: cross-connect + port lease

4. Kullanıcı portal'dan hesap doğrula:
   - Pre-Prod  : https://connectint3.borsaistanbul.com/
   - Prod-like : https://connectint2.borsaistanbul.com/
   - T+1       : https://connect-uat.borsaistanbul.com/

5. config/<env>.local.toml dosyasına credentials yaz

6. Preflight çalıştır:
   ./build/apps/tools/preflight config/bist_preprod.toml

7. Mock'ta çalışan scenarios'u live'da koş:
   ./build/apps/bist_colo --config config/bist_preprod.toml \
     --replay scenarios/ouch_bolum1_baglanti.yaml
```

## 7. Cert Akışı (Önerilen Sıra)

```
Faz 1: MOCK (mevcut)        ✅ tamam
       --mock --replay scenarios/

Faz 2: PRE-PROD iterasyon
       config/bist_preprod.toml
       Bölüm 1 → 2 → 3 sırası, sonra FIX OE → RD

Faz 3: PROD-LIKE dry-run
       config/bist_prod_like.toml
       2 ardışık temiz koşum (cert günü öncesi 1-2 hafta)

Faz 4: CERT günü
       BIST Borsa Eksperi gözlemli
       Prod-like COLO (194.0.142.76) üzerinden
```

## 8. Diğer Sistemler

### KİT (KMTP İşlem Terminali)

| Ortam | URL |
|---|---|
| Pre-Prod | `https://prova.borsaistanbul.com` |
| Prod-like | `https://kittest.borsaistanbul.com` |
| T+1 | `https://kmptest.borsaistanbul.com` |

### Gün Sonu Raporları

API: `https://verdaint2.borsaistanbul.com` (Prod-like), `https://verdaint3.borsaistanbul.com` (Pre-Prod), `verda-uat.borsaistanbul.com` (T+1).

### Takasbank & MKK Entegrasyonu

- Pre-Prod: ❌ yok
- Prod-like: ✅ var
- T+1: ✅ var

### BAP Tescil Test (10.57.3.43)

Bu projeye dahil değil. Kayıt için: `bap@borsaistanbul.com`. Web: `https://test-tescil.borsaistanbul.com`.

## 9. Değişiklik Bildirimi

BIST endpoint'leri zaman içinde değişebilir. Bu doküman 24.03.2026 baseline'ıyla yazılmıştır. Bağlantı sorununda:

```
1. ouchpdfs/txt/Test_Ortamlari_Bilgilendirme_Dokumani.txt güncel mi kontrol et
2. BIST teknik dokümanlar sayfasından son sürümü çek:
   https://borsaistanbul.com/teknik-kaynaklar/teknik-dokumanlar
3. Değişiklik varsa config/bist_*.toml ve bu doc güncelle
4. ouchpdfs/pdf/'e yeni PDF'i koy, txt regenerate et:
   pdftotext -layout new.pdf ouchpdfs/txt/Test_Ortamlari_Bilgilendirme_Dokumani.txt
```

## 10. İletişim

- **Teknik destek:** `bistechsupport_autoticket@borsaistanbul.com`
- **BAP:** `bap@borsaistanbul.com`
- **Teknik dokümanlar:** `https://borsaistanbul.com/teknik-kaynaklar/teknik-dokumanlar`
- **Mevzuat:** `https://borsaistanbul.com/kurumsal/mevzuat/prosedurler`
