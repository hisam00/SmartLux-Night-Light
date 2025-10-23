// server.js
const express = require('express');
const cors = require('cors');
const admin = require('firebase-admin');
const axios = require('axios');

const app = express();
app.use(cors());
app.use(express.json());

function preventColdStart() {
  // Get the URL Render assigns to your service from environment variables
  const RENDER_EXTERNAL_URL = process.env.RENDER_EXTERNAL_URL;
  if (!RENDER_EXTERNAL_URL) {
    console.log("RENDER_EXTERNAL_URL not set. Cold start prevention disabled.");
    return;
  }

  const pingUrl = `${RENDER_EXTERNAL_URL}/`; 

  // Ping every 10 minutes (600,000 milliseconds)
  setInterval(() => {
    axios.get(pingUrl)
      .then(() => console.log(`[Keep-Alive] Pinged self successfully at ${new Date().toLocaleTimeString()}`))
      .catch(err => console.error(`[Keep-Alive] Ping failed: ${err.message}`));
  }, 600000); 
}

if (!process.env.FIREBASE_SERVICE_ACCOUNT_BASE64) {
  console.error("FATAL: FIREBASE_SERVICE_ACCOUNT_BASE64 environment variable not set.");
  // Exit the process if the critical credential is not found
  process.exit(1);
}

try {
    // Decode the Base64 string from the environment variable back into a JSON string
    const serviceAccountJson = Buffer.from(
      process.env.FIREBASE_SERVICE_ACCOUNT_BASE64,
      'base64'
    ).toString();
    
    // Parse the JSON string into an object
    const serviceAccount = JSON.parse(serviceAccountJson);

    admin.initializeApp({
      credential: admin.credential.cert(serviceAccount),
    });
} catch (error) {
    console.error("FATAL: Failed to initialize Firebase Admin SDK from environment variable.", error);
    process.exit(1);
}
// =========================================================
// 🔄 ADJUSTMENT END
// =========================================================


const auth = admin.auth();
const firestore = admin.firestore();

// API: Create new user
app.post('/api/createUser', async (req, res) => {
  try {
    const { email, password, username, firstName, lastName, role } = req.body;
    if (!email || !password || !username) {
      return res.status(400).json({ error: "Missing fields." });
    }
    const userRecord = await auth.createUser({ email, password, displayName: username });
    await firestore.collection('users').doc(userRecord.uid).set({
      username, firstName, lastName, email, role, createdAt: admin.firestore.FieldValue.serverTimestamp(),
    });
    res.status(201).json({ uid: userRecord.uid, message: "User created!" });
  } catch (error) {
    console.error("Create User Error:", error);
    res.status(500).json({ error: error.message });
  }
});

// API: Update existing user's Auth email/displayName and Firestore user doc
app.post('/api/updateUser', async (req, res) => {
  try {
    const { uid, email, username, firstName, lastName, role } = req.body;
    if (!uid || !email || !username) {
      return res.status(400).json({ error: 'Missing required fields (uid, email, username).' });
    }

    // 1) Update Firebase Auth
    try {
      await auth.updateUser(uid, { email, displayName: username });
    } catch (err) {
      console.error('Auth update error for uid', uid, err);
      return res.status(500).json({ error: 'Failed to update Firebase Auth user: ' + err.message });
    }

    // 2) Update Firestore users collection
    const userRef = firestore.collection('users').doc(uid);
    await userRef.update({
      username,
      firstName,
      lastName,
      email,
      role,
      updatedAt: admin.firestore.FieldValue.serverTimestamp(),
    });

    res.status(200).json({ message: 'User updated successfully.' });
  } catch (error) {
    console.error("Update User Error:", error);
    res.status(500).json({ error: error.message });
  }
});

// API: Update existing user's password securely
app.post('/api/updateUserPassword', async (req, res) => {
  try {
    const { uid, newPassword } = req.body;
    if (!uid || !newPassword) {
      return res.status(400).json({ error: 'Missing user ID or new password.' });
    }
    await auth.updateUser(uid, { password: newPassword });
    res.status(200).json({ message: 'Password updated successfully.' });
  } catch (error) {
    console.error("Update Password Error:", error);
    res.status(500).json({ error: error.message });
  }
});

// NEW API: Delete user from Firebase Auth and Firestore
app.post('/api/deleteUser', async (req, res) => {
  try {
    const { uid } = req.body;
    if (!uid) {
      return res.status(400).json({ error: 'Missing uid.' });
    }

    // Attempt to delete from Firebase Auth
    let authDeleted = false;
    try {
      await auth.deleteUser(uid);
      authDeleted = true;
    } catch (authErr) {
      // If the user doesn't exist or other error, log it but continue to try Firestore delete.
      console.error(`Failed to delete Auth user ${uid}:`, authErr);
      // If the error is something other than 'user-not-found', we return an error so admin can investigate.
      if (authErr.code && authErr.code !== 'auth/user-not-found') {
        return res.status(500).json({ error: 'Failed to delete user in Firebase Auth: ' + authErr.message });
      }
      // if user-not-found, proceed to delete Firestore doc anyway
    }

    // Delete from Firestore
    try {
      await firestore.collection('users').doc(uid).delete();
    } catch (fsErr) {
      console.error(`Failed to delete Firestore doc for ${uid}:`, fsErr);
      // If Auth deletion succeeded but Firestore failed, respond with partial failure info
      if (authDeleted) {
        return res.status(500).json({ error: 'Auth user deleted but failed to delete Firestore doc: ' + fsErr.message });
      }
      return res.status(500).json({ error: 'Failed to delete Firestore doc: ' + fsErr.message });
    }

    res.status(200).json({ message: 'User deleted from Auth and Firestore (if present).' });
  } catch (error) {
    console.error("Delete User Error:", error);
    res.status(500).json({ error: error.message });
  }
});

// Basic root route for confirmation
app.get('/', (req, res) => {
  res.send('Backend server is running.');
});

// Add near other endpoints in server.js

// 1) Check user existence (admin lookup)
app.post('/api/checkUserByEmail', async (req, res) => {
  const { email } = req.body || {};
  if (!email) return res.status(400).json({ ok: false, error: 'email required' });
  try {
    const user = await auth.getUserByEmail(email);
    return res.json({
      ok: true,
      uid: user.uid,
      email: user.email,
      providers: (user.providerData || []).map(p => p.providerId),
    });
  } catch (err) {
    if (err.code === 'auth/user-not-found') {
      return res.json({ ok: false, code: 'user-not-found' });
    }
    console.error('Admin lookup error:', err);
    return res.status(500).json({ ok: false, error: err.message || err });
  }
});

// 2) Generate password reset link (and optionally send email)
// NOTE: In production you should send the link via your own email provider.
// For testing you can return the link in JSON (NOT recommended in production).
app.post('/api/sendResetLink', async (req, res) => {
  const { email, sendEmail } = req.body || {};
  if (!email) return res.status(400).json({ ok: false, error: 'email required' });

  try {
    // 1) Confirm user exists (throws if not)
    const user = await auth.getUserByEmail(email);

    // 2) Ensure user supports password sign-in
    const providers = (user.providerData || []).map(p => p.providerId);
    if (!providers.includes('password')) {
      // For security, you may want to respond with a generic message instead.
      return res.json({ ok: false, code: 'no-password-provider', providers });
    }

    // 3) Generate password reset link
    const link = await auth.generatePasswordResetLink(email);

    // Option A: return the link (for debugging/testing only)
    if (!sendEmail) {
      return res.json({ ok: true, uid: user.uid, link });
    }

  } catch (err) {
    console.error('sendResetLink error:', err);
    if (err.code === 'auth/user-not-found') {
      // For debugging we return code. In production, consider returning generic success.
      return res.json({ ok: false, code: 'user-not-found' });
    }
    return res.status(500).json({ ok: false, error: err.message || err });
  }
});


// ======= Middleware to verify Firebase ID token (Authorization: Bearer <idToken>) =======
async function verifyFirebaseToken(req, res, next) {
  try {
    const authHeader = req.headers.authorization || '';
    if (!authHeader.startsWith('Bearer ')) {
      return res.status(401).json({ ok: false, error: 'Missing or invalid Authorization header' });
    }
    const idToken = authHeader.split('Bearer ')[1].trim();
    const decoded = await admin.auth().verifyIdToken(idToken);
    req.user = { uid: decoded.uid, email: decoded.email, admin: !!decoded.admin };
    return next();
  } catch (err) {
    console.error('verify token failed', err);
    return res.status(401).json({ ok: false, error: 'Invalid token' });
  }
}

// ======= helper =======
function isValidUrl(u) {
  try {
    const parsed = new URL(u);
    return parsed.protocol === 'https:' || parsed.protocol === 'http:';
  } catch (e) {
    return false;
  }
}

// ======= Firestore shape (collection 'webhooks', doc.id == deviceId) =======
// { motion: [ { id, url, owner, createdAt, updatedAt }, ... ],
//  temperature: [ ... ] }

// helper to read subscriber URLs for a device/event (deduped)
async function getSubscribers(deviceId, eventType) {
  try {
    const doc = await firestore.collection('webhooks').doc(deviceId).get();
    if (!doc.exists) return [];
    const data = doc.data() || {};
    let subs = [];
    if (eventType === 'motion') subs = Array.isArray(data.motion) ? data.motion : [];
    else subs = Array.isArray(data.temperature) ? data.temperature : [];
    const urls = subs.map(s => s.url).filter(Boolean);
    return Array.from(new Set(urls)); // dedupe
  } catch (err) {
    console.error('getSubscribers error', err);
    return [];
  }
}

// ======= Add subscription (user-owned) =======
app.post('/api/webhooks/:deviceId', verifyFirebaseToken, async (req, res) => {
  try {
    const deviceId = req.params.deviceId;
    const { event, url } = req.body || {};
    if (!deviceId || !event || !url) return res.status(400).json({ ok: false, error: 'deviceId, event and url are required' });
    if (!isValidUrl(url)) return res.status(400).json({ ok: false, error: 'Invalid URL' });

    const eventKey = (event === 'motion') ? 'motion' : 'temperature';
    const ownerUid = req.user.uid;
    const subId = firestore.collection('webhooks').doc().id; // generate id

    const ref = firestore.collection('webhooks').doc(deviceId);
    await firestore.runTransaction(async (t) => {
      const snap = await t.get(ref);
      const data = snap.exists ? snap.data() : {};
      const arr = Array.isArray(data[eventKey]) ? data[eventKey].slice() : [];
      arr.push({
        id: subId,
        url,
        owner: ownerUid,
        createdAt: admin.firestore.Timestamp.now(),
      });
      t.set(ref, { ...data, [eventKey]: arr }, { merge: true });
    });

    return res.status(201).json({ ok: true, id: subId, message: 'Webhook added' });
  } catch (err) {
    console.error('Add webhook error', err);
    return res.status(500).json({ ok: false, error: err.message || err });
  }
});

// ======= Edit subscription (owner only) =======
app.patch('/api/webhooks/:deviceId/:subId', verifyFirebaseToken, async (req, res) => {
  try {
    const deviceId = req.params.deviceId;
    const subId = req.params.subId;
    const { url, event } = req.body || {};
    if (!deviceId || !subId) return res.status(400).json({ ok: false, error: 'deviceId and subId required' });
    if (url && !isValidUrl(url)) return res.status(400).json({ ok: false, error: 'Invalid URL' });

    let targetEventKey = null;
    if (typeof event === 'string') {
      if (event === 'motion') targetEventKey = 'motion';
      else if (event === 'temperature' || event === 'high_temperature') targetEventKey = 'temperature';
      else return res.status(400).json({ ok: false, error: 'Invalid event type' });
    }

    const uid = req.user.uid;
    const isAdmin = req.user.admin || false;
    const ref = firestore.collection('webhooks').doc(deviceId);

    const updated = await firestore.runTransaction(async (t) => {
      const snap = await t.get(ref);
      if (!snap.exists) throw { code: 404, message: 'Device webhooks not found' };
      const data = snap.data() || {};
      const keys = ['motion', 'temperature'];

      let foundKey = null, foundIndex = -1, foundObj = null;
      for (const k of keys) {
        const arr = Array.isArray(data[k]) ? data[k].slice() : [];
        const idx = arr.findIndex(s => s.id === subId);
        if (idx !== -1) {
          foundKey = k; foundIndex = idx; foundObj = arr[idx];
          break;
        }
      }
      if (!foundObj) throw { code: 404, message: 'Subscription not found' };
      if (!isAdmin && foundObj.owner !== uid) throw { code: 403, message: 'Forbidden: not owner' };

      const newData = Object.assign({}, data);
      for (const k of keys) newData[k] = Array.isArray(data[k]) ? data[k].slice() : [];

      if (url) {
        foundObj.url = url;
        foundObj.updatedAt = admin.firestore.Timestamp.now();
        newData[foundKey][foundIndex] = foundObj;
      }

      if (targetEventKey && targetEventKey !== foundKey) {
        newData[foundKey].splice(foundIndex, 1);
        const moved = Object.assign({}, foundObj);
        moved.updatedAt = admin.firestore.Timestamp.now();
        newData[targetEventKey].push(moved);
      }

      t.set(ref, newData, { merge: true });
      return { id: foundObj.id, url: (url || foundObj.url), owner: foundObj.owner, event: targetEventKey || foundKey };
    });

    return res.json({ ok: true, updated: updated });
  } catch (err) {
    console.error('Edit webhook error', err);
    if (err && err.code === 404) return res.status(404).json({ ok: false, error: err.message || 'Not found' });
    if (err && err.code === 403) return res.status(403).json({ ok: false, error: err.message || 'Forbidden' });
    return res.status(500).json({ ok: false, error: err.message || err });
  }
});

// ======= Delete subscription (owner only) =======
app.delete('/api/webhooks/:deviceId/:subId', verifyFirebaseToken, async (req, res) => {
  try {
    const deviceId = req.params.deviceId;
    const subId = req.params.subId;
    if (!deviceId || !subId) return res.status(400).json({ ok: false, error: 'deviceId and subId required' });

    const uid = req.user.uid;
    const isAdmin = req.user.admin || false;
    const ref = firestore.collection('webhooks').doc(deviceId);

    const success = await firestore.runTransaction(async (t) => {
      const snap = await t.get(ref);
      if (!snap.exists) return false;
      const data = snap.data();
      const keys = ['motion', 'temperature'];
      let found = false;
      const newData = Object.assign({}, data);
      for (const k of keys) {
        const arr = Array.isArray(data[k]) ? data[k].slice() : [];
        const idx = arr.findIndex(s => s.id === subId);
        if (idx !== -1) {
          if (!isAdmin && arr[idx].owner !== uid) throw new Error('Not authorized to delete this subscription');
          arr.splice(idx, 1);
          newData[k] = arr;
          found = true;
          break;
        }
      }
      if (!found) return false;
      t.set(ref, newData, { merge: true });
      return true;
    });

    if (!success) return res.status(404).json({ ok: false, error: 'Subscription not found' });
    return res.json({ ok: true, message: 'Subscription deleted' });
  } catch (err) {
    if (err.message && err.message.includes('Not authorized')) {
      return res.status(403).json({ ok: false, error: 'Forbidden' });
    }
    console.error('Delete webhook error', err);
    return res.status(500).json({ ok: false, error: err.message || err });
  }
});

// ======= List subscriptions (admin sees all; normal users see only their own) =======
app.get('/api/webhooks/:deviceId', verifyFirebaseToken, async (req, res) => {
  try {
    const deviceId = req.params.deviceId;
    const uid = req.user.uid;
    const isAdmin = req.user.admin || false;
    const doc = await firestore.collection('webhooks').doc(deviceId).get();
    if (!doc.exists) return res.json({ ok: true, motion: [], temperature: [] });
    const data = doc.data();

    if (isAdmin) return res.json({ ok: true, ...data });

    const motion = (data.motion || []).filter(s => s.owner === uid);
    const temperature = (data.temperature || []).filter(s => s.owner === uid);
    return res.json({ ok: true, motion, temperature });
  } catch (err) {
    console.error('List webhooks error', err);
    return res.status(500).json({ ok: false, error: err.message || err });
  }
});

// ======= Forwarding endpoint (ESP -> this server) =======
// Recommended: protect this endpoint with a query token. Set process.env.NOTIFY_TOKEN = 'LONGSECRET' in your env.
// ESP will read this full URL from RTDB (devices/<id>/notify/motion_url etc)
app.post('/api/notify/forward/:deviceId', async (req, res) => {
  try {
    // validate token if configured
    const expected = process.env.NOTIFY_TOKEN || '';
    console.log('[notify] token auth', expected ? 'ENABLED' : 'DISABLED');
    const token = req.query.token || '';
    console.log('[notify/forward] incoming request for deviceId=%s token=%s', req.params.deviceId, token);

    // Log useful debug info immediately
    console.log('[notify/forward] headers:', req.headers);
    // req.body should be available because you have app.use(express.json())
    console.log('[notify/forward] body (raw):', req.body);

    if (expected && token !== expected) {
      console.warn('[notify/forward] invalid or missing token');
      return res.status(401).json({ ok: false, error: 'invalid token' });
    }

    const deviceId = req.params.deviceId;
    const payload = req.body || {};

    // respond fast so ESP isn't blocked
    res.status(202).json({ ok: true, message: 'Accepted for forwarding' });

    // determine event type from payload
    const eventType = payload.event || payload.event_type || 'motion';
    const normalizedEvent = (eventType === 'high_temperature' || eventType === 'temperature') ? 'high_temperature' : 'motion';
    const enumKey = (normalizedEvent === 'motion') ? 'motion' : 'temperature';

    // fan-out asynchronously (non-blocking)
    (async () => {
      try {
        const subs = await getSubscribers(deviceId, enumKey);
        console.log('[notify/forward] found %d subscribers for device=%s event=%s', (subs || []).length, deviceId, enumKey);

        if (!subs || subs.length === 0) {
          console.log(`[notify/forward] no subscribers for ${deviceId} ${enumKey}`);
          return;
        }

        const axiosOpts = { timeout: 5000, headers: { 'Content-Type': 'application/json' } };
        const promises = subs.map(url =>
          axios.post(url, payload, axiosOpts)
               .then(r => ({ url, status: r.status, data: r.data }))
               .catch(e => ({ url, error: e.message || String(e) }))
        );

        const settled = await Promise.allSettled(promises);
        settled.forEach((r, i) => {
          if (r.status === 'fulfilled') {
            // r.value includes url/status/data for clarity
            console.log('[notify/forward] posted ->', subs[i], r.value);
          } else {
            // r.reason might have error information
            console.warn('[notify/forward] failed ->', subs[i], r.reason || r);
          }
        });
      } catch (err) {
        console.error('[notify/forward] fan-out error', err);
      }
    })();

  } catch (err) {
    console.error('[notify/forward] error', err);
    try { res.status(500).json({ ok: false, error: 'server error' }); } catch(e){/*ignore*/ }
  }
});


const HOST = '0.0.0.0';
// Render automatically provides a PORT environment variable. Use it!
const PORT = process.env.PORT || 3000; 

app.listen(PORT, HOST, () => {
    console.log(`Server running at http://${HOST}:${PORT}`);
    preventColdStart(); // <--- START THE PING LOOP HERE
});