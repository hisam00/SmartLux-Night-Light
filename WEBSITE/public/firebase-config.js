// firebase-config.js
import { initializeApp } from "https://www.gstatic.com/firebasejs/12.1.0/firebase-app.js";
import { getAuth, createUserWithEmailAndPassword } from "https://www.gstatic.com/firebasejs/12.1.0/firebase-auth.js";
import { getFirestore, doc, setDoc } from "https://www.gstatic.com/firebasejs/12.1.0/firebase-firestore.js";
import { getAnalytics } from "https://www.gstatic.com/firebasejs/12.1.0/firebase-analytics.js";
import { sendEmailVerification } from "https://www.gstatic.com/firebasejs/12.1.0/firebase-auth.js";

const firebaseConfig = {
  apiKey: "AIzaSyBu4mqM1PeTHW4AKoP3e8Nhcx4jiHtwueU",
  authDomain: "smartlux-night-light.firebaseapp.com",
  databaseURL: "https://smartlux-night-light-default-rtdb.asia-southeast1.firebasedatabase.app",
  projectId: "smartlux-night-light",
  storageBucket: "smartlux-night-light.firebasestorage.app",
  messagingSenderId: "558742892847",
  appId: "1:558742892847:web:30b85deeca4102cb7f71da",
  measurementId: "G-MD4BV2GWYF"
};

const app = initializeApp(firebaseConfig);
const auth = getAuth(app);
const db = getFirestore(app);
const analytics = getAnalytics(app);

export { auth, db, createUserWithEmailAndPassword, doc, setDoc, sendEmailVerification };
