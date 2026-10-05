// ota_page.h
// Страница /ota: обновление прошивки и раздела www из браузера.
// Перенесена из wifi-serv (src/wifi/update.h): заменены бренд, переменные темы
// (префикс pdm -> rg) и эндпоинты (/api/spiffs/update -> /api/www/update,
// /api/keys -> /api/status). Страница вшита в прошивку, а не лежит в www: она
// должна работать, даже когда раздел www пуст или перезаписывается.
#pragma once

const char RG_OTA_PAGE_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>roundGauge - Update</title>
    <link rel="stylesheet" href="/bootstrap.min.css">
    <link rel="stylesheet" href="/fontawesome/css/all.min.css">
    <style>
        :root {
            --rg-bg: #16181b;
            --rg-bg-elevated: #1f2226;
            --rg-bg-elevated-2: #262a2f;
            --rg-border: #34383f;
            --rg-text: #eceef0;
            --rg-text-muted: #8b939c;
            --rg-accent: #ffc933;
            --rg-accent-dark: #cf9f1a;
            --rg-accent-soft: rgba(255, 201, 51, 0.12);
            --rg-danger: #ff5c5c;
            --rg-success: #35d07f;
        }

        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }

        body {
            background: var(--rg-bg);
            color: var(--rg-text);
            min-height: 100vh;
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
        }

        .navbar {
            background: var(--rg-bg-elevated) !important;
            border-bottom: 1px solid var(--rg-border);
        }

        .navbar-brand {
            font-weight: 700;
            font-size: 1.5rem;
            color: var(--rg-text) !important;
        }

        .navbar-brand i {
            color: var(--rg-accent);
        }

        .main-container {
            max-width: 700px;
            margin: 80px auto 40px;
            padding: 0 20px;
        }

        .card {
            background: var(--rg-bg-elevated);
            border: 1px solid var(--rg-border);
            border-radius: 15px;
            margin-bottom: 30px;
            overflow: hidden;
        }

        .card-header {
            background: var(--rg-bg-elevated-2);
            color: var(--rg-text);
            border-bottom: 1px solid var(--rg-border);
            padding: 20px;
            font-size: 1.2rem;
            font-weight: 600;
            display: flex;
            align-items: center;
            gap: 10px;
        }

        .card-header i {
            color: var(--rg-accent);
        }

        .card-body {
            padding: 25px;
        }

        .status-bar {
            background: var(--rg-bg-elevated);
            border: 1px solid var(--rg-border);
            border-radius: 10px;
            padding: 12px 15px;
            margin-bottom: 18px;
            display: flex;
            align-items: center;
            gap: 10px;
            font-size: 0.9rem;
        }

        .status-dot {
            width: 10px;
            height: 10px;
            border-radius: 50%;
            background: var(--rg-text-muted);
        }

        .status-dot.connected { background: var(--rg-success); }
        .status-dot.uploading { background: var(--rg-accent); animation: pulse 1s infinite; }
        .status-dot.error { background: var(--rg-danger); }

        @keyframes pulse {
            0%, 100% { opacity: 1; }
            50% { opacity: 0.5; }
        }

        .upload-zone {
            border: 3px dashed var(--rg-border);
            border-radius: 15px;
            padding: 30px 20px;
            text-align: center;
            cursor: pointer;
            transition: all 0.3s ease;
            background: var(--rg-bg);
        }

        .upload-zone:hover {
            border-color: var(--rg-accent);
            background: var(--rg-accent-soft);
        }

        .upload-zone i {
            font-size: 2.5rem;
            color: var(--rg-accent);
            margin-bottom: 10px;
        }

        .upload-zone p {
            color: var(--rg-text-muted);
        }

        .file-name {
            font-weight: 600;
            margin-top: 10px;
            display: none;
            color: var(--rg-text);
        }

        .btn {
            border-radius: 10px;
            padding: 10px 24px;
            font-weight: 600;
        }

        .btn-primary {
            background: var(--rg-accent);
            border: none;
            color: #1a1a1a;
        }

        .btn-primary:hover {
            background: var(--rg-accent-dark);
            color: #1a1a1a;
        }

        .btn-primary:disabled {
            background: var(--rg-bg-elevated-2);
            color: var(--rg-text-muted);
        }

        .btn-outline-light {
            border: 1px solid var(--rg-border);
            color: var(--rg-text);
            background: transparent;
        }

        .btn-outline-light:hover {
            background: var(--rg-bg-elevated-2);
            color: var(--rg-text);
        }

        .alert {
            border-radius: 10px;
            padding: 12px 18px;
            margin-top: 15px;
            display: none;
            border: 1px solid var(--rg-border);
        }

        .alert.show {
            display: flex;
            align-items: center;
            gap: 10px;
        }

        .alert-success {
            background: rgba(53, 208, 127, 0.12);
            color: var(--rg-success);
        }

        .alert-danger {
            background: rgba(255, 92, 92, 0.12);
            color: var(--rg-danger);
        }

        input[type="file"] {
            display: none;
        }

        .upload-progress {
            height: 8px;
            border-radius: 4px;
            background: var(--rg-bg-elevated-2);
            overflow: hidden;
            margin-bottom: 18px;
            display: none;
        }

        .upload-progress.show {
            display: block;
        }

        .upload-progress-bar {
            height: 100%;
            width: 0;
            background: var(--rg-accent);
            transition: width 0.2s ease;
        }
    </style>
</head>
<body>
    <nav class="navbar navbar-dark fixed-top">
        <div class="container">
            <a class="navbar-brand" href="/">
                <i class="fas fa-gauge-high"></i> roundGauge
            </a>
            <a href="/" class="btn btn-outline-light btn-sm">
                <i class="fas fa-arrow-left"></i> Back to control
            </a>
        </div>
    </nav>

    <div class="main-container">
        <div class="status-bar">
            <div class="status-dot" id="statusDot"></div>
            <span id="statusText">Ready</span>
        </div>
        <div class="upload-progress" id="uploadProgress">
            <div class="upload-progress-bar" id="uploadProgressBar"></div>
        </div>

        <!-- Firmware update -->
        <div class="card">
            <div class="card-header"><i class="fas fa-microchip"></i> Firmware update</div>
            <div class="card-body">
                <div class="upload-zone" onclick="document.getElementById('fwInput').click()">
                    <i class="fas fa-cloud-upload-alt"></i>
                    <p><strong>Choose a firmware file</strong> (.bin)</p>
                    <div class="file-name" id="fwFileName"></div>
                </div>
                <input type="file" id="fwInput" accept=".bin" onchange="onFileSelected(event, 'fw')">
                <button class="btn btn-primary w-100 mt-3" id="fwBtn" disabled onclick="startUpdate('fw')">
                    <i class="fas fa-upload"></i> Flash device
                </button>
            </div>
        </div>

        <!-- Web interface update -->
        <div class="card">
            <div class="card-header"><i class="fas fa-globe"></i> Web interface update</div>
            <div class="card-body">
                <div class="upload-zone" onclick="document.getElementById('webInput').click()">
                    <i class="fas fa-cloud-upload-alt"></i>
                    <p><strong>Choose a filesystem image</strong> (www.bin)</p>
                    <div class="file-name" id="webFileName"></div>
                </div>
                <input type="file" id="webInput" accept=".bin" onchange="onFileSelected(event, 'web')">
                <button class="btn btn-primary w-100 mt-3" id="webBtn" disabled onclick="startUpdate('web')">
                    <i class="fas fa-upload"></i> Update files
                </button>
            </div>
        </div>

        <div class="alert" id="alertMessage"></div>
    </div>

    <script>
        const files = { fw: null, web: null };
        const endpoints = { fw: '/api/ota/update', web: '/api/www/update' };
        const labels = { fw: 'the firmware', web: 'the web interface files' };

        function onFileSelected(event, kind) {
            const file = event.target.files[0];
            if (!file) return;
            files[kind] = file;
            const nameEl = document.getElementById(kind + 'FileName');
            nameEl.textContent = file.name + ' (' + formatSize(file.size) + ')';
            nameEl.style.display = 'block';
            document.getElementById(kind + 'Btn').disabled = false;
        }

        // XMLHttpRequest, а не fetch: у fetch нет прогресса отправки тела, и на
        // образе в несколько мегабайт страница минуту стояла на "Uploading..." без
        // единого признака жизни. Плюс явный таймаут - если ответ так и не придёт
        // (сорвалась связь с точкой доступа), пользователь увидит это, а не вечное
        // ожидание.
        const UPLOAD_TIMEOUT_MS = 5 * 60 * 1000;

        function upload(url, file, onProgress) {
            const xhr = new XMLHttpRequest();
            const promise = new Promise((resolve, reject) => {
                xhr.open('POST', url);
                xhr.setRequestHeader('Content-Type', 'application/octet-stream');
                xhr.timeout = UPLOAD_TIMEOUT_MS;
                xhr.upload.onprogress = (e) => {
                    if (e.lengthComputable) onProgress(e.loaded / e.total);
                };
                xhr.onload = () => resolve({ status: xhr.status, text: xhr.responseText });
                xhr.onerror = () => reject(new Error('connection lost'));
                xhr.ontimeout = () => reject(new Error('no response from the device'));
                xhr.onabort = () => reject(new Error('aborted'));
                xhr.send(file);
            });
            return { xhr, promise };
        }

        // Короткий опрос платы с таймаутом: пока она принимает образ, её однопоточный
        // HTTP-сервер занят и не ответит - это нормально, просто спросим ещё раз.
        async function fetchWithTimeout(url, ms) {
            const ctrl = new AbortController();
            const t = setTimeout(() => ctrl.abort(), ms);
            try {
                return await fetch(url, { cache: 'no-store', signal: ctrl.signal });
            } finally {
                clearTimeout(t);
            }
        }

        // Хеш содержимого веб-интерфейса на плате (/.build_id в www). Генератор
        // из wifi-serv (tools/build/gen_build_id.*) пока не перенесён - без
        // него здесь всегда null, и остаётся только основной признак успеха.
        async function fetchBuildId() {
            try {
                const r = await fetchWithTimeout('/.build_id', 3000);
                return r.ok ? (await r.text()).trim() : null;
            } catch (e) {
                return null;
            }
        }

        // Итог загрузки с нашим id из /api/status (поле www_update, webcfg_task.c).
        // null - плата ещё занята или итога с таким id нет.
        async function fetchUpdateResult(id) {
            try {
                const r = await fetchWithTimeout('/api/status', 3000);
                if (!r.ok) return null;
                const u = (await r.json()).www_update;
                return (u && u.id === id && u.done) ? u : null;
            } catch (e) {
                return null;
            }
        }

        // Результат загрузки, не зависящий от ответа на неё. Safari на iPhone обрывает
        // соединение, пока плата ещё принимает и сравнивает образ, и XMLHttpRequest у
        // него при этом не завершается ни ответом, ни ошибкой - страница висела на
        // счётчике, хотя файлы записаны. Поэтому параллельно спрашиваем плату:
        //   - основной признак: итог с нашим id в /api/status - работает всегда, в том
        //     числе когда загружен тот же образ, что уже стоит;
        //   - запасной: новый /.build_id - если плата успела записать файлы и
        //     перезагрузилась, итог в её памяти пропал, а файлы новые.
        function watchResult(id, buildIdBefore) {
            let stop = false;
            const promise = (async () => {
                while (!stop) {
                    await new Promise(r => setTimeout(r, 3000));
                    if (stop) break;
                    const u = await fetchUpdateResult(id);
                    if (u) return u;
                    if (buildIdBefore !== null) {
                        const b = await fetchBuildId();
                        if (b !== null && b !== buildIdBefore) return { done: true, ok: true };
                    }
                }
                return null;
            })();
            return { promise, cancel: () => { stop = true; } };
        }

        function showWebSuccess(u) {
            setProgress(1);
            setStatus('connected', 'Files updated');
            let stats = '';
            if (u && typeof u.blocks_written === 'number') {
                const total = u.blocks_written + u.blocks_skipped;
                stats = ' Rewritten ' + u.blocks_written + ' of ' + total +
                    ' blocks, ' + (u.total_ms / 1000).toFixed(1) + ' s on the device.';
            }
            showAlert('Web interface files updated.' + stats + ' Reloading page...', 'success');
            setTimeout(() => { window.location.href = '/'; }, 3000);
        }

        function setProgress(fraction) {
            document.getElementById('uploadProgress').classList.add('show');
            document.getElementById('uploadProgressBar').style.width =
                Math.round(fraction * 100) + '%';
        }

        // После перезагрузки плате нужно несколько секунд, чтобы поднять точку доступа,
        // а телефону или ноутбуку - чтобы переподключиться к ней. Поэтому не прыгаем на
        // главную по таймеру вслепую, а ждём, пока плата реально ответит.
        async function waitForDevice() {
            for (let i = 0; i < 60; i++) {
                await new Promise(r => setTimeout(r, 2000));
                try {
                    const r = await fetch('/api/status', { cache: 'no-store' });
                    if (r.ok) return true;
                } catch (e) { /* ещё не поднялась */ }
            }
            return false;
        }

        // Ответа нет - спрашиваем саму плату, что с ней: жива ли и не перезагружалась ли
        // за время обновления. uptime_s меньше времени с начала загрузки означает
        // перезагрузку посреди процесса, а reset_reason - её причину.
        async function diagnose(startedAt) {
            try {
                const r = await fetch('/api/status', { cache: 'no-store' });
                if (!r.ok) return ' The device answers, but with HTTP ' + r.status + '.';
                const d = await r.json();
                const elapsed = (Date.now() - startedAt) / 1000;
                if (typeof d.uptime_s === 'number' && d.uptime_s < elapsed) {
                    return ' The device REBOOTED during the update ' + Math.round(d.uptime_s) +
                        ' s ago (reason: ' + (d.reset_reason || 'unknown') + '). Reload the page to see which files it has now.';
                }
                return ' The device is running and did not reboot, but its reply was lost. Reload the page to check whether the update applied.';
            } catch (e) {
                return ' The device is not reachable right now - reconnect to its Wi-Fi network and reload the page.';
            }
        }

        async function startUpdate(kind) {
            const file = files[kind];
            if (!file) return;
            if (!confirm('Update ' + labels[kind] + '?')) return;

            setStatus('uploading', 'Uploading... 0%');
            setProgress(0);
            document.getElementById(kind + 'Btn').disabled = true;

            let uploaded = false;
            let answered = false; // плата сообщила итог - ответом или через /api/status
            const startedAt = Date.now();
            // Процент отправки считает браузер по тому, что ушло в сокет его ОС, а не по
            // тому, что плата уже записала: 100% наступает, когда файл лёг в буфер
            // отправки, и дальше плата ещё долго принимает и пишет его. Чтобы этот этап
            // не выглядел зависанием, показываем, сколько он уже длится.
            let writeTimer = null;
            const stopWriteTimer = () => { if (writeTimer) { clearInterval(writeTimer); writeTimer = null; } };

            // Для файлов интерфейса: id загрузки, по которому плата публикует итог, и
            // текущий хеш - запасной признак успеха (см. watchResult).
            const updateId = Math.random().toString(36).slice(2, 12);
            let url = endpoints[kind];
            if (kind === 'web') url += (url.includes('?') ? '&' : '?') + 'id=' + updateId;
            const buildIdBefore = kind === 'web' ? await fetchBuildId() : null;
            let watcher = null;

            try {
                const up = upload(url, file, (f) => {
                    setProgress(f);
                    if (f < 1) {
                        setStatus('uploading', 'Uploading... ' + Math.round(f * 100) + '%');
                    } else if (!uploaded) {
                        uploaded = true;
                        const writeStart = Date.now();
                        setStatus('uploading', 'Sent, the device is receiving and writing it... 0 s');
                        writeTimer = setInterval(() => {
                            setStatus('uploading', 'Sent, the device is receiving and writing it... ' +
                                Math.round((Date.now() - writeStart) / 1000) + ' s');
                        }, 1000);
                    }
                });

                let res;
                if (kind === 'web') {
                    watcher = watchResult(updateId, buildIdBefore);
                    const first = await Promise.race([
                        up.promise.then(r => ({ via: 'response', r })),
                        watcher.promise.then(u => ({ via: 'device', u }))
                    ]);
                    watcher.cancel();
                    if (first.via === 'device' && first.u) {
                        // Итог уже известен от самой платы - ответ на загрузку не нужен.
                        up.xhr.abort();
                        stopWriteTimer();
                        answered = true;
                        if (!first.u.ok) throw new Error(first.u.error || 'Update failed');
                        showWebSuccess(first.u);
                        return;
                    }
                    res = first.r;
                } else {
                    res = await up.promise;
                }
                stopWriteTimer();

                answered = true;
                let result = {};
                try { result = JSON.parse(res.text); } catch (e) { /* ошибка приходит текстом */ }
                if (res.status !== 200 || result.status !== 'success') {
                    // Плата присылает короткий текст ("Write failed"); если пришла
                    // HTML-страница или что-то длинное - показываем только код.
                    const text = (res.text || '').trim();
                    const msg = (text && text.length < 120 && !text.includes('<'))
                        ? text : ('HTTP ' + res.status);
                    throw new Error(msg);
                }

                if (kind === 'fw') {
                    setStatus('uploading', 'Success! Rebooting...');
                    showAlert('Firmware updated, the device is rebooting. Waiting for it to come back...', 'success');
                    const back = await waitForDevice();
                    if (back) {
                        window.location.href = '/';
                    } else {
                        setStatus('error', 'Device did not come back');
                        showAlert('The device has not responded for 2 minutes. Reconnect to its Wi-Fi network and open the page again.', 'danger');
                    }
                } else {
                    showWebSuccess(result);
                }
            } catch (err) {
                let error = err;
                if (watcher) watcher.cancel();
                stopWriteTimer();
                // Соединение оборвалось, а итога ещё нет: плата могла дописать образ уже
                // после обрыва - спрашиваем её, прежде чем объявлять ошибку.
                if (kind === 'web' && uploaded && !answered) {
                    setStatus('uploading', 'Connection lost, asking the device for the result...');
                    for (let i = 0; i < 15; i++) {
                        const u = await fetchUpdateResult(updateId);
                        if (u) {
                            if (u.ok) { showWebSuccess(u); return; }
                            error = new Error(u.error || 'Update failed');
                            answered = true;
                            break;
                        }
                        const b = buildIdBefore !== null ? await fetchBuildId() : null;
                        if (b !== null && b !== buildIdBefore) { showWebSuccess(null); return; }
                        await new Promise(r => setTimeout(r, 2000));
                    }
                }
                setStatus('error', 'Update error');
                // Итога нет вовсе - это не то же самое, что ошибка записи: файлы вполне
                // могли записаться. Спрашиваем плату, что произошло.
                let hint = '';
                if (!answered) {
                    showAlert('Error: ' + error.message + '. Checking the device...', 'danger');
                    hint = await diagnose(startedAt);
                }
                showAlert('Error: ' + error.message + '.' + hint, 'danger');
                document.getElementById(kind + 'Btn').disabled = false;
            }
        }

        function setStatus(type, message) {
            document.getElementById('statusDot').className = 'status-dot ' + type;
            document.getElementById('statusText').textContent = message;
        }

        function showAlert(message, type) {
            const alert = document.getElementById('alertMessage');
            alert.className = 'alert alert-' + type + ' show';
            alert.innerHTML = '<i class="fas fa-' +
                (type === 'success' ? 'check-circle' : 'exclamation-circle') + '"></i> ' + message;
        }

        function formatSize(bytes) {
            if (bytes === 0) return '0 Bytes';
            const k = 1024;
            const sizes = ['Bytes', 'KB', 'MB'];
            const i = Math.floor(Math.log(bytes) / Math.log(k));
            return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
        }

        setStatus('connected', 'Ready');
    </script>
</body>
</html>
)rawliteral";
