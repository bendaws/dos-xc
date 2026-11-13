'use strict';
(() => {
  const form = document.getElementById('publish-form');
  if (!form || !window.fetch || !window.Blob || !Blob.prototype.slice) return;
  const input = form.elements.package;
  const button = form.querySelector('button[type=submit]');
  const list = document.getElementById('upload-progress');
  const csrf = form.elements.csrf.value;
  const base = form.dataset.uploads;
  const maxSize = Number(form.dataset.maxSize);
  const maxFiles = Number(form.dataset.maxFiles);
  const pause = ms => new Promise(resolve => setTimeout(resolve, ms));

  async function call(method, url, body, json) {
    const headers = {'X-CSRF-Token': csrf};
    if (json) headers['Content-Type'] = 'application/json';
    const response = await fetch(url, {method, headers, body, credentials: 'same-origin'});
    let data = {};
    try { data = await response.json(); } catch (_) { /* Proxies may answer with HTML. */ }
    if (!response.ok) {
      const error = new Error(data.error || `The server answered HTTP ${response.status}.`);
      error.status = response.status;
      throw error;
    }
    return data;
  }

  // Failed chunks resume from the offset the server committed.
  async function send(file, progress) {
    const upload = await call('POST', base, JSON.stringify({size: file.size}), true);
    const url = `${base}/${upload.id}`;
    let offset = 0;
    let failures = 0;
    try {
      while (offset < file.size) {
        const end = Math.min(offset + upload.chunk_size, file.size);
        try {
          offset = (await call('PUT', `${url}?offset=${offset}`, file.slice(offset, end))).received;
          failures = 0;
        } catch (error) {
          const retry = !error.status || error.status === 409 || error.status >= 500;
          if (!retry || ++failures > 5) throw error;
          await pause(1000 * 2 ** failures);
          offset = (await call('GET', url)).received;
        }
        progress(offset / file.size);
      }
      return await call('POST', `${url}/complete`);
    } catch (error) {
      call('DELETE', url).catch(() => {});
      throw error;
    }
  }

  function row(file) {
    const item = document.createElement('li');
    const name = document.createElement('strong');
    const bar = document.createElement('progress');
    const status = document.createElement('span');
    name.textContent = file.name;
    bar.max = 1;
    bar.value = 0;
    status.textContent = 'Waiting';
    item.append(name, bar, status);
    list.append(item);
    return {item, bar, status};
  }

  form.addEventListener('submit', async event => {
    event.preventDefault();
    const files = Array.from(input.files);
    if (!files.length) return;
    if (files.length > maxFiles) {
      alert(`Publish up to ${maxFiles} packages at once.`);
      return;
    }
    button.disabled = input.disabled = true;
    list.replaceChildren();
    list.hidden = false;
    const rows = files.map(row);
    for (const [index, file] of files.entries()) {
      const {item, bar, status} = rows[index];
      try {
        if (!file.name.toLowerCase().endsWith('.dsp')) throw new Error('Choose a .dsp package file.');
        if (!file.size) throw new Error('The file is empty.');
        if (file.size > maxSize) throw new Error(`Packages can be up to ${Math.floor(maxSize / 1048576)} MiB.`);
        status.textContent = 'Uploading';
        const result = await send(file, fraction => {
          bar.value = fraction;
          status.textContent = fraction < 1 ? `Uploading ${Math.floor(fraction * 100)}%` : 'Checking package';
        });
        const link = document.createElement('a');
        link.href = `/packages/${result.id}`;
        link.textContent = `${result.name} ${result.version} (${result.arch}-bit)`;
        status.replaceChildren('Published ', link);
        item.className = 'done';
      } catch (error) {
        status.textContent = error.message || 'The upload failed.';
        item.className = 'failed';
      }
      bar.remove();
    }
    form.reset();
    button.disabled = input.disabled = false;
  });
})();
