'use strict';
const dialog = document.getElementById('project-dialog');
const title = document.getElementById('dialog-title');
const copy = document.getElementById('dialog-copy');
const action = dialog.querySelector('.dialog-action');
let trigger;
let purchase = false;
document.querySelectorAll('[data-dialog="buy"]').forEach(button => {
  button.addEventListener('click', () => {
    trigger = button;
    purchase = true;
    const name = button.querySelector('.offer-name').textContent;
    const price = button.querySelector('.offer-price').textContent;
    const contents = document.getElementById(button.getAttribute('aria-describedby'));
    const checkoutNote = 'Shipping across Europe included. Checkout preview only.';
    title.textContent = `${name} · ${price}`;
    copy.textContent = contents ? `${contents.textContent} ${checkoutNote}` : checkoutNote;
    action.textContent = 'Preview checkout ↗';
    dialog.showModal();
  });
});
action.addEventListener('click', () => {
  if (!purchase) { dialog.close(); return; }
  purchase = false;
  copy.textContent = `${title.textContent} including European shipping. No order placed or payment taken.`;
  title.textContent = 'Checkout preview complete.';
  action.textContent = 'Back';
});
dialog.querySelector('.close').addEventListener('click', () => dialog.close());
dialog.addEventListener('close', () => trigger?.focus());
const dashboard = document.getElementById('dashboard-dialog');
const dashboardButton = document.getElementById('show-dashboard');
dashboardButton.addEventListener('click', () => dashboard.showModal());
dashboard.querySelector('.close').addEventListener('click', () => dashboard.close());
dashboard.addEventListener('close', () => dashboardButton.focus());

const hardwareCaption = document.getElementById('hardware-caption');
const hardwareNote = document.getElementById('hardware-note');
const caseDownloads = document.getElementById('case-downloads');
const hardwareViews = {
  photo: {
    element: document.getElementById('hardware-photo'),
    caption: 'brain_zero in its housing',
    note: 'VE.Bus RJ45 · Power and data'
  },
  exploded: {
    element: document.getElementById('hardware-exploded'),
    ready: false,
    caption: 'PCB inside · Lid lifted',
    downloads: true
  },
  pcb: {
    element: document.getElementById('hardware-pcb'),
    ready: false,
    caption: 'PCB · Rev C · 31 fitted parts'
  },
  case: {
    element: document.getElementById('hardware-case'),
    ready: false,
    caption: 'Print your own case',
    downloads: true
  }
};
let activeHardwareView = 'photo';
function updateHardwareNote() {
  const view = hardwareViews[activeHardwareView];
  if (view.note) {
    hardwareNote.textContent = view.note;
    return;
  }
  hardwareNote.textContent = view.error ? '3D unavailable. Showing the CAD render.' : view.ready ? 'Drag to rotate · Scroll or pinch to zoom' : 'Loading 3D…';
}
document.querySelectorAll('[data-hardware-view]').forEach(button => {
  button.addEventListener('click', () => {
    activeHardwareView = button.dataset.hardwareView;
    const view = hardwareViews[activeHardwareView];
    Object.entries(hardwareViews).forEach(([name, item]) => { item.element.hidden = name !== activeHardwareView; });
    hardwareCaption.textContent = view.caption;
    caseDownloads.hidden = !view.downloads;
    updateHardwareNote();
    document.querySelectorAll('[data-hardware-view]').forEach(item => {
      item.setAttribute('aria-pressed', String(item === button));
    });
  });
});
Object.values(hardwareViews).forEach(view => {
  if (view.note) return;
  view.element.addEventListener('load', () => { view.ready = true; updateHardwareNote(); });
  view.element.addEventListener('error', () => { view.error = true; updateHardwareNote(); });
});
